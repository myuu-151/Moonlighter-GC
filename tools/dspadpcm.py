"""The GameCube's DSP-ADPCM, as MLGC stores it (Source/AudioDsp.h decodes it).

A frame is 14 samples in 8 bytes: a header byte (the predictor, 0-7, in the high nibble and
the scale's shift in the low one), then 14 signed 4-bit residuals, the high nibble first. A
sample is

    s = ((residual << shift) << 11) + 1024 + c1 * yn1 + c2 * yn2;  s >>= 11;  clamped to 16 bits

with (c1, c2) the predictor's coefficients (1.11 fixed point) and yn1, yn2 the two samples
before it, as decoded.

MLGC's clips are cut in blocks of 512 frames (7168 samples) a channel, each with its own
64-byte header: the 8 coefficient pairs fitted to that block, and the two samples before it.
Its first frame starts from those samples as they were before encoding, and the encoder starts
from the same ones, so a block decodes on its own, exactly as encoded: the blocks are encoded
side by side (numpy, many at a time; the frames of a block one after another), and a stream
can start, loop or seek at any block.

    header (big-endian): coefs[8][2] s16 | yn1 s16 | yn2 s16 | frames u16 | zeros to 64 bytes
    then `frames` frames of 8 bytes

The encoding of a frame is the usual DSPADPCM encoder's (the scale search and rounding of
the one in the public domain tools); the coefficients are fitted per block by k-means on each
frame's prediction error (splitting 1 -> 2 -> 4 -> 8).
"""
import numpy as np

FRAME = 14
BLOCK_FRAMES = 512
BLOCK_SAMPLES = FRAME * BLOCK_FRAMES
HEADER_BYTES = 64
FRAME_BYTES = 8
CHANNEL_BLOCK_BYTES = HEADER_BYTES + BLOCK_FRAMES * FRAME_BYTES  # 4160, a multiple of 32
# The encoder's rounding constant (a float literal, 0.4999999f, used as a double).
HALF = float(np.float32(0.4999999))


def _solve(R11, R12, R22, r1, r2):
    """The 2-tap predictor that minimises the error with these sums (regularised)."""
    eps = 1e-9 * (R11 + R22) + 1e-3
    A = R11 + eps
    D = R22 + eps
    det = A * D - R12 * R12
    det = np.where(np.abs(det) < 1e-12, 1e-12, det)
    a1 = (r1 * D - r2 * R12) / det
    a2 = (r2 * A - r1 * R12) / det
    return np.clip(a1, -7.9, 7.9), np.clip(a2, -7.9, 7.9)


def coefficients(x, hist, iters=5):
    """Each block's 8 predictors: x (B, S) samples, hist (B, 2) the two before ([yn2, yn1]).
    -> (B, 8, 2) int64 (c1, c2) in 1.11 fixed point, within +-16383 (sums stay in 32 bits)."""
    B, S = x.shape
    F = S // FRAME
    full = np.concatenate([hist, x], axis=1).astype(np.float64)
    s0, s1, s2 = full[:, 2:], full[:, 1:-1], full[:, :-2]

    def fsum(a):
        return a.reshape(B, F, FRAME).sum(axis=2)

    # Each frame's sums: its prediction error with (c1, c2) is stats . [1, -2c1, -2c2, c1^2, 2c1c2, c2^2].
    E0, r1, r2, R11, R12, R22 = (fsum(s0 * s0), fsum(s0 * s1), fsum(s0 * s2), fsum(s1 * s1), fsum(s1 * s2),
                                 fsum(s2 * s2))
    stats = np.stack([E0, r1, r2, R11, R12, R22], axis=2)  # (B, F, 6)
    pf1, pf2 = _solve(R11, R12, R22, r1, r2)  # each frame's own (to re-seed an empty cluster)

    def error(c1, c2):  # (B, K) -> (B, F, K)
        terms = np.stack([np.ones_like(c1), -2 * c1, -2 * c2, c1 * c1, 2 * c1 * c2, c2 * c2], axis=1)  # (B, 6, K)
        return np.matmul(stats, terms)

    c1, c2 = _solve(R11.sum(1), R12.sum(1), R22.sum(1), r1.sum(1), r2.sum(1))
    c1, c2 = c1[:, None], c2[:, None]
    bi = np.arange(B)
    sums_of = np.stack([R11, R12, R22, r1, r2], axis=1)  # (B, 5, F)
    while c1.shape[1] < 8:
        c1 = np.concatenate([c1 * 1.01 + 0.01, c1 * 0.99 - 0.01], axis=1)
        c2 = np.concatenate([c2 * 1.01 - 0.01, c2 * 0.99 + 0.01], axis=1)
        K = c1.shape[1]
        for _ in range(iters):
            e = error(c1, c2)
            nearest = e.argmin(axis=2)
            onehot = (nearest[..., None] == np.arange(K)).astype(np.float64)  # (B, F, K)
            sums = np.matmul(sums_of, onehot)  # (B, 5, K)
            count = onehot.sum(axis=1)
            n1, n2 = _solve(*(sums[:, i] for i in range(5)))
            worst = e.min(axis=2).argmax(axis=1)
            c1 = np.where(count > 0, n1, pf1[bi, worst][:, None])
            c2 = np.where(count > 0, n2, pf2[bi, worst][:, None])
    coefs = np.stack([np.round(c1 * 2048), np.round(c2 * 2048)], axis=2)
    return np.clip(coefs, -16383, 16383).astype(np.int64)


def _tdiv(a, b):
    """C's integer division (toward zero)."""
    q = np.abs(a) // b
    return np.where(a < 0, -q, q)


def encode(x, hist, coefs, candidates=3):
    """The blocks' frames: x (B, S) samples, hist (B, 2) [yn2, yn1], coefs (B, 8, 2).
    -> (headers (B, F) uint8, residuals (B, F, 14) int8, decoded (B, S) int16).

    The scale search runs for the `candidates` predictors (of 8) that predict the frame best
    before quantising (the reference encoder tries all 8: 3 come within about half a dB of it, in
    less than half the time). Every block's candidates side by side as lanes; in 32-bit integers
    throughout (the products and sums fit: coefficients within +-16383). The rounding,
    trunc(v2 / 2^k +- 0.4999999f), is sign(v2) * ((|v2| + H) >> k) with H = floor(h * 2^k)
    (h * 2^k is never a whole number for k < 25, so the two agree for every v2)."""
    B, S = x.shape
    F = S // FRAME
    K = candidates
    L = B * K
    x = x.astype(np.int32)
    h2 = hist[:, 0].astype(np.int32)
    h1 = hist[:, 1].astype(np.int32)
    lane_block = np.repeat(np.arange(B), K)
    half = np.array([16777213 >> (25 - k) for k in range(32 - 7)] + [0] * 7, np.int32)  # H by k (k < 25)
    headers = np.zeros((B, F), np.uint8)
    residuals = np.zeros((B, F, FRAME), np.int8)
    decoded = np.zeros((B, S), np.int16)
    bi = np.arange(B)
    for f in range(F):
        P = x[:, f * FRAME:(f + 1) * FRAME]  # (B, 14)
        buf = np.concatenate([h2[:, None], h1[:, None], P], axis=1).astype(np.int64)
        # The largest residual, predicting from the samples as they are: the starting scale.
        v1 = _tdiv(buf[:, None, :-2] * coefs[:, :, 1, None] + buf[:, None, 1:-1] * coefs[:, :, 0, None], 2048)
        v3 = np.clip(buf[:, None, 2:] - v1, -32768, 32767)
        # The candidates: the predictors with the least error before quantising.
        pick = np.argsort((v3 * v3).sum(axis=2), axis=1, kind='stable')[:, :K]  # (B, K)
        v3 = np.take_along_axis(v3, pick[..., None], axis=1)
        c1 = np.take_along_axis(coefs[:, :, 0], pick, axis=1).astype(np.int32).reshape(L)
        c2 = np.take_along_axis(coefs[:, :, 1], pick, axis=1).astype(np.int32).reshape(L)
        at = np.abs(v3).argmax(axis=2)
        distance = np.take_along_axis(v3, at[..., None], axis=2)[..., 0].reshape(L)
        scale = np.zeros(L, np.int32)
        for _ in range(13):
            m = (scale <= 12) & ((distance > 7) | (distance < -8))
            if not m.any():
                break
            scale += m
            distance = np.where(m, _tdiv(distance, 2), distance)
        scale = np.where(scale <= 1, -1, scale - 2).astype(np.int32)
        # Up the scale until the residuals fit, quantising against the decoded samples; each
        # round only the lanes still going.
        out = np.zeros((L, FRAME), np.int32)
        ins = np.zeros((L, FRAME), np.int32)
        acc = np.zeros(L, np.int64)
        lanes = np.arange(L)
        rounds = 0
        while len(lanes) and rounds < 24:  # (the search can't go round for ever)
            rounds += 1
            sc = scale[lanes] + 1
            scale[lanes] = sc
            blk = lane_block[lanes]
            k1 = c1[lanes]
            k2 = c2[lanes]
            i0 = h2[blk]
            i1 = h1[blk]
            k = sc + 11
            H = half[k]
            index = np.zeros(len(lanes), np.int32)
            o = np.empty((len(lanes), FRAME), np.int32)
            n = np.empty((len(lanes), FRAME), np.int32)
            a = np.zeros(len(lanes), np.int64)
            Pl = P[blk]
            for s_ in range(FRAME):
                p = Pl[:, s_]
                v1 = i0 * k2 + i1 * k1
                v2 = (p << 11) - v1
                v3 = (np.abs(v2) + H) >> k
                v3 = np.where(v2 > 0, v3, -v3)
                over = np.maximum(np.maximum(-8 - v3, v3 - 7), 0)
                np.maximum(index, over, out=index)
                np.minimum(np.maximum(v3, -8, out=v3), 7, out=v3)
                o[:, s_] = v3
                r = (v1 + ((v3 << sc) << 11) + 1024) >> 11
                np.minimum(np.maximum(r, -32768, out=r), 32767, out=r)
                n[:, s_] = r
                d = (p - r).astype(np.int64)
                a += d * d
                i0, i1 = i1, r
            out[lanes] = o
            ins[lanes] = n
            acc[lanes] = a
            wide = index + 8
            s2 = sc.copy()
            for _ in range(8):
                m = wide > 256
                if not m.any():
                    break
                s2 = np.where(m, np.where(s2 + 1 >= 12, 11, s2 + 1), s2)
                wide = np.where(m, wide >> 1, wide)
            scale[lanes] = s2
            lanes = lanes[(s2 < 12) & (index > 1)]
        best = acc.reshape(B, K).argmin(axis=1)
        chosen = bi * K + best
        predictor = pick[bi, best]
        headers[:, f] = ((predictor << 4) | (scale[chosen] & 0xF)).astype(np.uint8)
        residuals[:, f] = out[chosen]
        d = ins[chosen]
        decoded[:, f * FRAME:(f + 1) * FRAME] = d
        h2, h1 = d[:, 12].copy(), d[:, 13].copy()
    return headers, residuals, decoded


def decode(headers, residuals, hist, coefs):
    """What the GameCube decodes (Source/AudioDsp.h), for checking."""
    B, F = headers.shape
    out = np.zeros((B, F * FRAME), np.int64)
    h2 = hist[:, 0].copy()
    h1 = hist[:, 1].copy()
    bi = np.arange(B)
    for f in range(F):
        ci = headers[:, f].astype(np.int64) >> 4
        shift = headers[:, f].astype(np.int64) & 0xF
        k1 = coefs[bi, ci, 0]
        k2 = coefs[bi, ci, 1]
        for s in range(FRAME):
            v = np.clip((((residuals[:, f, s].astype(np.int64) << shift) << 11) + 1024 + k1 * h1 + k2 * h2) >> 11,
                        -32768, 32767)
            out[:, f * FRAME + s] = v
            h2, h1 = h1, v
    return out


def split(channel):
    """A channel's samples (int16) as blocks: (x (B, BLOCK_SAMPLES) int64, hist (B, 2) int64,
    frames per block)."""
    n = len(channel)
    count = max(1, (n + BLOCK_SAMPLES - 1) // BLOCK_SAMPLES)
    padded = np.zeros(count * BLOCK_SAMPLES + 2, np.int64)
    padded[2:2 + n] = channel
    x = padded[2:].reshape(count, BLOCK_SAMPLES)
    starts = np.arange(count) * BLOCK_SAMPLES
    hist = np.stack([padded[starts], padded[starts + 1]], axis=1)  # the two before each block
    frames = [min(BLOCK_FRAMES, max(0, (n - b * BLOCK_SAMPLES + FRAME - 1) // FRAME)) for b in range(count)]
    return x, hist, frames


def encode_blocks(x, hist, check=True):
    """Encodes blocks (from split, any number of channels and clips side by side)."""
    coefs = coefficients(x, hist)
    headers, residuals, decoded = encode(x, hist, coefs)
    if check and not np.array_equal(decode(headers, residuals, hist.astype(np.int64), coefs), decoded):
        raise SystemExit('DSP-ADPCM: the decoder disagrees with the encoder')
    return coefs, headers, residuals, decoded


def block_bytes(coefs, hist, frames, headers, residuals, full=False):
    """One channel's block as stored: its header, then its frames (all 512 with `full`, else
    `frames`), padded to 32 bytes."""
    head = np.zeros(HEADER_BYTES // 2, '>i2')
    head[:16] = coefs.reshape(16)
    head[16] = hist[1]  # yn1
    head[17] = hist[0]  # yn2
    head[18] = frames
    count = BLOCK_FRAMES if full else frames
    body = np.zeros((count, FRAME_BYTES), np.uint8)
    body[:, 0] = headers[:count]
    r = residuals[:count] & 0xF
    body[:, 1:] = (r[:, 0::2] << 4) | r[:, 1::2]
    data = head.tobytes() + body.tobytes()
    return data + bytes(-len(data) % 32)
