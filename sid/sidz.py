"""SIDZ: compact SID register-frame stream for playback straight from flash.

Repeated passages are encoded as "calls" back into the stream, so the board
needs no RAM history: it follows calls with a small stack (like subroutines).
A call is only emitted where the SID register state before both passages is
identical, so replay is exact.

Token stream (first byte):
  0x00         one frame with no register changes
  0x01-0x0F    frame; bits = register groups present (v1 0-6, v2 7-13,
               v3 14-20, filter/volume 21-24); one mask byte per group
               (bit n = register base+n), then the values in register order
  0x10-0x7F    run of (b - 0x0E) empty frames (2..113)
  0x80         call: u24 target offset, u16 frame count (little endian)
  0xFF         end of tune (loop back to the start)
"""
import struct

GROUPS = ((0, 7), (7, 7), (14, 7), (21, 4))
MAX_DEPTH = 6
MIN_CALL = 4          # frames


def parse_sidl(log):
    """SIDL log -> (frame_us, [(mask, values bytes), ...])"""
    frame_us, n = struct.unpack("<HI", log[6:12])
    p, frames = 12, []
    for _ in range(n):
        mask = struct.unpack("<I", log[p:p + 4])[0] & 0x1FFFFFF
        k = bin(mask).count("1")
        frames.append((mask, bytes(log[p + 4:p + 4 + k])))
        p += 4 + k
    return frame_us, frames


def encode_frame(mask, vals):
    if not mask:
        return b"\x00"
    gm, sub = 0, bytearray()
    for g, (base, cnt) in enumerate(GROUPS):
        m = (mask >> base) & ((1 << cnt) - 1)
        if m:
            gm |= 1 << g
            sub.append(m)
    return bytes([gm]) + bytes(sub) + vals


def compress(frames):
    n = len(frames)
    # SID state before each frame
    state, states = bytearray(25), []
    for mask, vals in frames:
        states.append(bytes(state))
        k = 0
        for r in range(25):
            if mask >> r & 1:
                state[r] = vals[k]
                k += 1
    keys = [(states[i], frames[i]) for i in range(n)]
    index = {}                      # key -> list of earlier positions
    out = bytearray()
    pos_of = [0] * (n + 1)          # stream offset where frame i's encoding starts
    depth = [0] * n                 # decoder call depth needed to produce frame i
    i = 0
    while i < n:
        pos_of[i] = len(out)
        best_len, best_j = 0, -1
        for j in index.get(keys[i], ())[-64:]:
            L = 0
            while (i + L < n and j + L < i and frames[j + L] == frames[i + L]
                   and max(depth[j:j + L + 1]) < MAX_DEPTH):
                L += 1
            if L > best_len:
                best_len, best_j = L, j
        raw_cost = sum(len(encode_frame(*frames[i + k])) for k in range(min(best_len, MIN_CALL)))
        if best_len >= MIN_CALL and raw_cost > 6:
            out += b"\x80" + struct.pack("<I", pos_of[best_j])[:3] + struct.pack("<H", best_len)
            for k in range(best_len):
                depth[i + k] = depth[best_j + k] + 1
                index.setdefault(keys[i + k], []).append(i + k)
                if k:
                    pos_of[i + k] = len(out)        # not a valid call target
            # frames inside a call are not referenceable by offset
            for k in range(1, best_len):
                index[keys[i + k]].pop()
            i += best_len
            continue
        if frames[i][0] == 0:                       # run of empty frames
            r = 1
            while i + r < n and frames[i + r][0] == 0 and r < 113:
                r += 1
            if r >= 2:
                out.append(0x0E + r)
                for k in range(r):
                    pos_of[i + k] = len(out) - 1 if k == 0 else len(out)
                i += r
                continue
        out += encode_frame(*frames[i])
        index.setdefault(keys[i], []).append(i)
        i += 1
    out.append(0xFF)
    return bytes(out)


def decompress(data, limit=None):
    """Reference decoder (mirrors the board): yields (mask, values)."""
    stack = [[0, None]]             # [pos, frames remaining]
    produced = 0
    pending_empty = 0
    while limit is None or produced < limit:
        top = stack[-1]
        if pending_empty:
            pending_empty -= 1
            frame = (0, b"")
        else:
            b = data[top[0]]
            if b == 0xFF:
                return
            if b == 0x80:
                tgt = data[top[0] + 1] | data[top[0] + 2] << 8 | data[top[0] + 3] << 16
                cnt = data[top[0] + 4] | data[top[0] + 5] << 8
                top[0] += 6
                stack.append([tgt, cnt])
                continue
            if 0x10 <= b <= 0x7F:
                top[0] += 1
                pending_empty = b - 0x0E - 1
                frame = (0, b"")
            elif b == 0:
                top[0] += 1
                frame = (0, b"")
            else:
                p = top[0] + 1
                mask = 0
                for g, (base, cnt) in enumerate(GROUPS):
                    if b >> g & 1:
                        mask |= data[p] << base
                        p += 1
                k = bin(mask).count("1")
                frame = (mask, bytes(data[p:p + k]))
                top[0] = p + k
        yield frame
        produced += 1
        for lvl in stack[1:]:
            lvl[1] -= 1
        for d in range(1, len(stack)):
            if stack[d][1] <= 0:
                del stack[d:]
                pending_empty = 0 if d == 1 or True else pending_empty
                break
