#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The host-side mirror of radiant_core/src/radiant_sec_aes.c.

    C:\\ncs\\toolchains\\dcbdc366a1\\opt\\bin\\python.exe -m unittest tools.test_radiant_crypto

AES-128, AES-CMAC (RFC 4493), AES-CTR (SP 800-38A) and the SP 800-108
counter-mode KDF, in pure Python with no dependency of any kind. The toolchain
interpreter has pyusb and nothing else, so "no dependency" is not a style
preference - a `pip install pycryptodome` here is a tool that does not run.

Two reasons this exists rather than being a wrapper around a library:

  * `native_sim` does not build on Windows, so every C assertion in this
    project runs only in CI on Linux. This module is the layer a developer can
    iterate against locally, and tools/test_radiant_crypto.py runs the same
    published vectors the ztest suite does - FIPS-197, RFC 4493, SP 800-38A
    F.5.1 and, for the two blocks this protocol pins itself, a cross-check
    against the C implementation's own output.
  * The host tools need this anyway. `tools/ant_verify.py` cannot tell "replay
    dropped" from "packet lost" without being able to compute a tag, and a
    two-dongle 1:N proof needs a host that can decrypt.

DO NOT USE THIS FOR ANYTHING BUT RADIANT. It is a clear, slow, textbook AES
with no timing-attack hardening whatsoever: the S-box is a table lookup, and
`bytes` objects are copied freely. That is fine for a test oracle on a desktop
and wrong for anything holding a secret an attacker can time.

docs/radiant-security.md sections 3.1 to 3.4 are normative for every layout
here.
"""

from __future__ import annotations

# ── AES-128, encrypt only ───────────────────────────────────────────────────
#
# The same choice the C makes and for the same reason: CTR needs only the
# forward direction, CMAC needs only the forward direction, and a decrypt path
# is code and a table nothing calls.

SBOX = bytes.fromhex(
    "637c777bf26b6fc53001672bfed7ab76"
    "ca82c97dfa5947f0add4a2af9ca472c0"
    "b7fd9326363ff7cc34a5e5f171d83115"
    "04c723c31896059a071280e2eb27b275"
    "09832c1a1b6e5aa0523bd6b329e32f84"
    "53d100ed20fcb15b6acbbe394a4c58cf"
    "d0efaafb434d338545f9027f503c9fa8"
    "51a3408f929d38f5bcb6da2110fff3d2"
    "cd0c13ec5f974417c4a77e3d645d1973"
    "60814fdc222a908846eeb814de5e0bdb"
    "e0323a0a4906245cc2d3ac629195e479"
    "e7c8376d8dd54ea96c56f4ea657aae08"
    "ba78252e1ca6b4c6e8dd741f4bbd8b8a"
    "703eb5664803f60e613557b986c11d9e"
    "e1f8981169d98e949b1e87e9ce5528df"
    "8ca1890dbfe6426841992d0fb054bb16"
)

RCON = (0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36)


def _rotl32(word: int, bits: int) -> int:
    return ((word << bits) | (word >> (32 - bits))) & 0xFFFFFFFF


def _sub_word(word: int) -> int:
    return (
        (SBOX[(word >> 24) & 0xFF] << 24)
        | (SBOX[(word >> 16) & 0xFF] << 16)
        | (SBOX[(word >> 8) & 0xFF] << 8)
        | SBOX[word & 0xFF]
    )


def _xtime_word(word: int) -> int:
    """Multiply every byte of the word by x in GF(2^8)."""
    high = word & 0x80808080
    return (((word & 0x7F7F7F7F) << 1) ^ ((high >> 7) * 0x1B)) & 0xFFFFFFFF


def _mix_column(col: int) -> int:
    """d = 2*c ^ 3*r1 ^ r2 ^ r3 = xtime(c ^ r1) ^ r1 ^ r2 ^ r3.

    The identity that removes the four 1 KB T-tables. Same one the C uses; the
    two implementations agreeing on it is not a coincidence worth relying on,
    which is why both run FIPS-197's vectors.
    """
    r1 = _rotl32(col, 8)
    r2 = _rotl32(col, 16)
    r3 = _rotl32(col, 24)
    return _xtime_word(col ^ r1) ^ r1 ^ r2 ^ r3


def key_schedule(key: bytes) -> list:
    """The 44 round-key words of AES-128."""
    if len(key) != 16:
        raise ValueError("AES-128 takes a 16-byte key, got %d" % len(key))
    words = [int.from_bytes(key[i * 4:i * 4 + 4], "big") for i in range(4)]
    for i in range(4, 44):
        temp = words[i - 1]
        if i % 4 == 0:
            temp = _sub_word(_rotl32(temp, 8)) ^ (RCON[i // 4 - 1] << 24)
        words.append(words[i - 4] ^ temp)
    return words


def encrypt_block(key: bytes, block: bytes) -> bytes:
    """One AES-128 block encryption."""
    if len(block) != 16:
        raise ValueError("AES operates on 16-byte blocks, got %d" % len(block))
    words = key_schedule(key)
    col = [int.from_bytes(block[j * 4:j * 4 + 4], "big") ^ words[j]
           for j in range(4)]

    for rnd in range(1, 11):
        col = [_sub_word(c) for c in col]
        # ShiftRows: row r of the state is the byte at shift 24-8r of every
        # column word, and row r rotates left by r, so column j takes its
        # row-r byte from column (j+r) mod 4.
        shifted = [
            (col[j] & 0xFF000000)
            | (col[(j + 1) % 4] & 0x00FF0000)
            | (col[(j + 2) % 4] & 0x0000FF00)
            | (col[(j + 3) % 4] & 0x000000FF)
            for j in range(4)
        ]
        if rnd != 10:
            col = [_mix_column(shifted[j]) ^ words[4 * rnd + j]
                   for j in range(4)]
        else:
            col = [shifted[j] ^ words[4 * rnd + j] for j in range(4)]

    return b"".join(c.to_bytes(4, "big") for c in col)


# ── AES-CMAC, RFC 4493 ──────────────────────────────────────────────────────


def _dbl(block: bytes) -> bytes:
    value = int.from_bytes(block, "big")
    shifted = (value << 1) & ((1 << 128) - 1)
    if value >> 127:
        shifted ^= 0x87
    return shifted.to_bytes(16, "big")


def cmac_subkeys(key: bytes) -> tuple:
    """(K1, K2), from L = AES(K, 0^128)."""
    left = encrypt_block(key, bytes(16))
    k1 = _dbl(left)
    return k1, _dbl(k1)


def cmac(key: bytes, message: bytes) -> bytes:
    """The full 16-byte tag. Callers truncate; truncation is a protocol
    decision and never a primitive's."""
    k1, k2 = cmac_subkeys(key)

    if len(message) != 0 and len(message) % 16 == 0:
        blocks = [message[i:i + 16] for i in range(0, len(message), 16)]
        last = bytes(a ^ b for a, b in zip(blocks[-1], k1))
        blocks = blocks[:-1]
    else:
        padded = message + b"\x80" + bytes((-len(message) - 1) % 16)
        blocks = [padded[i:i + 16] for i in range(0, len(padded), 16)]
        last = bytes(a ^ b for a, b in zip(blocks[-1], k2))
        blocks = blocks[:-1]

    chain = bytes(16)
    for block in blocks:
        chain = encrypt_block(key, bytes(a ^ b for a, b in zip(chain, block)))
    return encrypt_block(key, bytes(a ^ b for a, b in zip(chain, last)))


# ── AES-CTR, SP 800-38A ─────────────────────────────────────────────────────


def ctr_xor(key: bytes, iv: bytes, data: bytes) -> bytes:
    """XOR `data` with the keystream from initial counter block `iv`.

    The counter is incremented AS A 128-BIT BIG-ENDIAN INTEGER, which is what
    SP 800-38A F.5's vectors do. RadiANT's nonce block ends in seven zero
    bytes, so for any payload this protocol carries that is indistinguishable
    from incrementing the last byte - but pinning it is what stops two
    implementations diverging on the first long message.
    """
    if len(iv) != 16:
        raise ValueError("the counter block is 16 bytes, got %d" % len(iv))
    out = bytearray()
    counter = int.from_bytes(iv, "big")
    for offset in range(0, len(data), 16):
        stream = encrypt_block(key, counter.to_bytes(16, "big"))
        chunk = data[offset:offset + 16]
        out.extend(a ^ b for a, b in zip(chunk, stream))
        counter = (counter + 1) & ((1 << 128) - 1)
    return bytes(out)


# ── The two blocks this protocol pins to the byte ───────────────────────────
#
# docs/radiant-security.md section 3.3. Two implementations that differ here do
# not interoperate, and the difference is invisible until it is a field
# failure - which is why it is pinned in the spec, checked by
# scripts/check_profile_registry.py, and written twice here and in C.

DOM_CTR = 0x01
DOM_SPREAD_MAC = 0x02
DOM_DESC_MAC = 0x03

#: docs/decisions/0008 section "What is pinned", and docs/radiant-security.md
#: section 11.4. A compat channel is an ANT+ channel, so its attestation shares
#: nothing with the spread tag but the key hierarchy - and without a domain byte
#: of its own a compat tag and a spread tag over the same counter can coincide.
COMPAT_DOM = 0x04

#: The subtype, which lives INSIDE the MAC'd nonce at position 9 and not only in
#: the page. Both tiers ride one page number, so a subtype an attacker can
#: rewrite in the page would leave a Tier I and a Tier II tag over the same
#: counter value separated by nothing at all. 0x03 is the SWITCH/RETURN
#: announcement's, pinned here so the value cannot be re-used before the layer
#: that emits it exists.
COMPAT_SUBTYPE_TIER_I = 0x01
COMPAT_SUBTYPE_TIER_II = 0x02
COMPAT_SUBTYPE_ANNOUNCE = 0x03

LABEL_ENC = "enc"
LABEL_AUTH = "auth"
LABEL_ID = "id"
LABEL_CMD = "cmd"

#: The legal MAC windows. W divides both 256 and 65536, which is the whole
#: reason a counter-derived window boundary resynchronises after a lost packet.
LEGAL_W = (2, 4, 8)
DEFAULT_W = 2

#: The secured page range, bounded so the descriptor (0x00) and the ANT+ common
#: pages (0x50/0x51/0x52) stay in the clear mechanically rather than by memory.
PAGE_LO_MIN = 0x01
PAGE_HI_MAX = 0x1F

#: Tier I is 40 bits and Tier II is 48, and the difference is not a security
#: judgement about the two tiers: Tier I carries a two-byte counter in the page
#: because no window index is implied, and 8 - 1 - 2 = 5 bytes are what is left.
#: 2^-40 per attempt against a mechanism rate-limited to one attempt per T by
#: construction is not the weak link.
COMPAT_TIER_I_TAG_BITS = 40
COMPAT_TIER_II_TAG_BITS = 48

#: Tier II's legal window sizes, and the switch countdown's legal lengths. Both
#: enumerated rather than ranged: N is simultaneously the airtime cost, the
#: verification latency and the DoS amplification factor.
COMPAT_WINDOW_SIZES = (4, 8, 16, 32)
COMPAT_COUNTDOWN_LENGTHS = (16, 32, 64, 128)
COMPAT_DEFAULT_WINDOW = 8


def nonce_block(epoch: int, devnum: int, counter: int, dom: int) -> bytes:
    """epoch[4 LE] || devnum[2 LE] || counter[2 LE] || dom || 0x00 x7."""
    return (
        (epoch & 0xFFFFFFFF).to_bytes(4, "little")
        + (devnum & 0xFFFF).to_bytes(2, "little")
        + (counter & 0xFFFF).to_bytes(2, "little")
        + bytes([dom & 0xFF])
        + bytes(7)
    )


def kdf_block(label: str, epoch: int, base_devnum: int) -> bytes:
    """0x01 || label || 0x00 || epoch[4 LE] || base_devnum[2 LE] || 0x0080."""
    if not 1 <= len(label) <= 4:
        raise ValueError("label %r is not one of enc/auth/id/cmd" % label)
    return (
        b"\x01"
        + label.encode("ascii")
        + b"\x00"
        + (epoch & 0xFFFFFFFF).to_bytes(4, "little")
        + (base_devnum & 0xFFFF).to_bytes(2, "little")
        + b"\x00\x80"
    )


def kdf(root: bytes, label: str, epoch: int = 0, base_devnum: int = 0) -> bytes:
    """One 128-bit derived key. The epoch-less "id" label uses epoch = 0."""
    if label == LABEL_ID and epoch != 0:
        raise ValueError('the "id" label is epoch-less and uses epoch = 0')
    return cmac(root, kdf_block(label, epoch, base_devnum))


def derive_keys(root: bytes, epoch: int, base_devnum: int) -> dict:
    """Every key of the hierarchy, from the one 16-byte root a pairing moves."""
    return {
        LABEL_ENC: kdf(root, LABEL_ENC, epoch, base_devnum),
        LABEL_AUTH: kdf(root, LABEL_AUTH, epoch, base_devnum),
        LABEL_ID: kdf(root, LABEL_ID, 0, base_devnum),
        LABEL_CMD: kdf(root, LABEL_CMD, epoch, base_devnum),
    }


# ── The two transforms ──────────────────────────────────────────────────────


def conf_transform(k_enc: bytes, epoch: int, devnum: int, counter: int,
                   payload: bytes) -> bytes:
    """X_CONF, both directions - CTR is its own inverse.

    Bytes [0] and [1] stay in the clear because a receiver needs the page
    number to tell a data page from a descriptor and the counter to build the
    nonce. Byte [7] is left alone too: with X_AUTH on it is the tag byte, and
    with X_AUTH off nothing there is a field, because the trailing byte is
    reserved tag space on every RadiANT page.
    """
    if len(payload) != 8:
        raise ValueError("a RadiANT page is 8 bytes, got %d" % len(payload))
    iv = nonce_block(epoch, devnum, counter, DOM_CTR)
    body = ctr_xor(k_enc, iv, payload[2:7])
    return payload[:2] + body + payload[7:]


def spread_tag(k_auth: bytes, epoch: int, devnum: int, window_start: int,
               packets) -> bytes:
    """X_AUTH's W-byte tag over one window, encrypt-then-MAC.

    `packets` is W packets AS THEY APPEAR ON THE AIR - ciphertext when X_CONF
    is on, plaintext when it is not - and each contributes bytes [0..6]. Byte
    [7] is excluded because that is where the tag itself rides.

    Byte [0] is inside the message on purpose: leave the page number out and an
    attacker who flips it reinterprets the same authenticated bits against a
    different schema.

    The nonce uses the counter of the FIRST packet of the window and domain
    byte 0x02, and the returned tag is W bytes - 8*W bits - not a fixed 32.
    """
    window = len(packets)
    if window not in LEGAL_W:
        raise ValueError("W must be one of %s, got %d" % (LEGAL_W, window))
    message = nonce_block(epoch, devnum, window_start, DOM_SPREAD_MAC)
    for packet in packets:
        if len(packet) != 8:
            raise ValueError("a RadiANT page is 8 bytes, got %d" % len(packet))
        message += bytes(packet[0:7])
    return cmac(k_auth, message)[:window]


# ── Compat attestation: two tiers over one domain byte ──────────────────────
#
# The mirror of radiant_core/src/radiant_sec_compat.c. docs/radiant-security.md
# section 11.4 is normative and docs/decisions/0008 pins the bytes.
#
# LIKE THE C, NOTHING HERE KNOWS A PAGE NUMBER OR A FIELD NAME. These take
# bytes and return bytes; which page carries them, and what the other bytes of
# that page mean, is tools/ant_pages.py's problem and not this module's. That is
# what makes the same two functions serve heart rate, power and a profile nobody
# has written yet.


def compat_nonce_block(epoch: int, devnum: int, counter: int,
                       sub: int) -> bytes:
    """epoch[4 LE] || devnum[2 LE] || counter[2 LE] || 0x04 || sub || 0x00 x6.

    Section 3.3's block extended at position 9 rather than duplicated - the
    domain byte stays where every other block has it, positions 10..15 stay
    zero, and the subtype is inside the MAC'd block rather than only in the page
    byte that announces it.

    `counter` is the attestation counter for Tier I and the window index for
    Tier II. Both are 16-bit here and both are carried truncated in the page,
    which is what lets a receiver reconstruct the high bits from time exactly as
    resolve_counter() already does for the data counter.
    """
    if not 1 <= sub <= 0x0F:
        raise ValueError("the subtype is a nibble in 1..15, got %r" % (sub,))
    return nonce_block(epoch, devnum, counter, COMPAT_DOM)[:9] \
        + bytes([sub]) + bytes(6)


def compat_tier1_tag(k_auth: bytes, epoch: int, devnum: int,
                     att_counter: int) -> bytes:
    """Tier I, the identity attestation: trunc40( CMAC(K_auth, nonce_block) ).

    IT COVERS NO PAYLOAD, AND THAT IS THE POINT. The tag proves "this stream
    comes from the holder of K_auth, now, and is not a replay" and nothing else,
    so it is verifiable on receipt and a packet lost anywhere else costs
    nothing - verification rate equals page delivery rate, independently of how
    often the page is sent. A window CMAC cannot have that property at any
    length, which is the whole reason this tier exists and is the one that is on
    by default.

    Replay is closed by `att_counter`, which is monotone and derivable from
    elapsed time; a receiver rejects a counter it has already seen.
    """
    block = compat_nonce_block(epoch, devnum, att_counter,
                               COMPAT_SUBTYPE_TIER_I)
    return cmac(k_auth, block)[:COMPAT_TIER_I_TAG_BITS // 8]


def compat_tier2_tag(k_auth: bytes, epoch: int, devnum: int,
                     window_index: int, messages) -> bytes:
    """Tier II, the data attestation:

        trunc48( CMAC(K_auth, nonce_block || p_1 || p_2 || ... || p_{N-1}) )

    `messages` is the N-1 preceding TRANSMITTED messages - not N-1 data
    messages, so the common pages, the beacon and any Tier I page in the window
    are covered too - in transmission order, each the full 8 payload bytes with
    the page number included. Leaving the page number out would let an attacker
    who flips it reinterpret the same authenticated bits against a different
    schema, and including it is also what keeps this function ignorant of what
    any of those bytes mean.

    Contrast spread_tag(), which takes bytes [0..6] because byte [7] is where
    its own tag rides. Nothing rides in a compat page's byte [7]: the tag has a
    page of its own, so all eight bytes are covered.

    The honest regression, and the reason this tier is off by default: a window
    CMAC is not self-synchronising under loss, so one lost packet in the window
    makes the whole window unverifiable.
    """
    window = len(messages) + 1
    if window not in COMPAT_WINDOW_SIZES:
        raise ValueError("N must be one of %s, so `messages` is N-1 = %s "
                         "messages, got %d"
                         % (COMPAT_WINDOW_SIZES,
                            tuple(n - 1 for n in COMPAT_WINDOW_SIZES),
                            len(messages)))
    block = compat_nonce_block(epoch, devnum, window_index,
                               COMPAT_SUBTYPE_TIER_II)
    for message in messages:
        if len(message) != 8:
            raise ValueError("a transmitted message is 8 bytes, got %d"
                             % len(message))
        block += bytes(message)
    return cmac(k_auth, block)[:COMPAT_TIER_II_TAG_BITS // 8]


def compat_announce_tag(k_auth: bytes, epoch: int, devnum: int,
                        att_counter: int, frame_a) -> bytes:
    """The SWITCH/RETURN announcement's tag, over frame A and nothing else:

        trunc48( CMAC(K_auth, nonce_block || frame_A) ), sub = 0x03

    Subtype 0x03 was pinned by C2 without a function to go with it, so that the
    value could not be re-used before the layer that emits it existed. This is
    that layer's half. `att_counter` is Tier I's counter, which is what makes a
    replayed announcement fail: the same frame A in a later counter's nonce
    produces a different tag, and a receiver that has already seen the counter
    rejects it before checking anything else.

    It is deliberately NOT compat_tier2_tag() with a window of one. That
    function's N must be a legal window size and its subtype says "this covers
    the last N-1 transmitted messages", which is a different claim about
    different bytes; sharing the code would have meant sharing the subtype, and
    the subtype is the only thing keeping the three claims apart.
    """
    if len(frame_a) != 8:
        raise ValueError("a transmitted frame is 8 bytes, got %d" % len(frame_a))
    block = compat_nonce_block(epoch, devnum, att_counter,
                               COMPAT_SUBTYPE_ANNOUNCE) + bytes(frame_a)
    return cmac(k_auth, block)[:COMPAT_TIER_II_TAG_BITS // 8]


#: The inbound command's subtype, and the one that is NOT under K_auth: a
#: command is verified under K_cmd, which had no user anywhere until Layer C's
#: trigger needed one. Two separations rather than one - the key stops a
#: recorded attestation being replayed into the command path however the bytes
#: are rearranged, and the subtype separates it inside the block anyway, because
#: "it is under a different key" is a property of the deployment rather than of
#: the block.
COMPAT_SUBTYPE_COMMAND = 0x04

#: 40 bits, because a command carries three bytes of its own before the tag and
#: 8 - 3 = 5 is what is left.
COMPAT_CMD_TAG_BITS = 40
COMPAT_CMD_BYTES = 3


def compat_command_tag(k_cmd: bytes, epoch: int, devnum: int,
                       att_counter: int, command) -> bytes:
    """An inbound command's tag: trunc40( CMAC(K_cmd, nonce_block || cmd) ).

    THE NODE IS THE VERIFIER, which is the opposite direction from every other
    tag here, and it is why the key is different: a receiver that can verify the
    node's attestation must not thereby be able to forge a command to it.

    `att_counter` is Tier I's counter again, derived from time on both sides and
    carried nowhere, so a recorded command replayed into a later interval fails
    the tag rather than being noticed afterwards.
    """
    if len(command) != COMPAT_CMD_BYTES:
        raise ValueError("a command covers %d bytes before its tag, got %d"
                         % (COMPAT_CMD_BYTES, len(command)))
    block = compat_nonce_block(epoch, devnum, att_counter,
                               COMPAT_SUBTYPE_COMMAND) + bytes(command)
    return cmac(k_cmd, block)[:COMPAT_CMD_TAG_BITS // 8]


#: The beacon's key-group hint is three bytes wide: enough for a receiver
#: holding a handful of roots to skip the ones that cannot match, and not enough
#: to be an identifier. The 24 bits are not a security claim - a collision is
#: expected every ~16 million epochs and is resolved by the attestation tag,
#: which is 40 or 48 bits and cannot be survived by a hint collision.
COMPAT_HINT_BITS = 24


def compat_key_group_hint(k_id: bytes, epoch: int) -> bytes:
    """trunc24( CMAC(K_id, epoch) ) - the beacon's "is this one of mine".

    EPOCH-DERIVED, NEVER STATIC, and that is the whole design of the field. A
    fixed "RadiANT, group ABC" byte string broadcast every 30 s would be a
    better tracking identifier than the device number, which is the same lesson
    that makes page 81's serial 0xFFFFFFFF under a privacy posture.

    It is also the epoch anchor that replaced the epoch field: a receiver whose
    last_seen_epoch is stale searches forward, one CMAC per candidate, until a
    hint matches - which is cheaper than the field it replaced and is paid once
    at re-acquisition rather than on every message.
    """
    return cmac(k_id, (epoch & 0xFFFFFFFF).to_bytes(4, "little"))[
        :COMPAT_HINT_BITS // 8]


#: The derived private-mode locator. "priv" is a CMAC MESSAGE PREFIX and not a
#: fifth KDF label: a KDF block is sixteen bytes beginning 0x01 and this message
#: is eight or nine beginning 'p', so the two inputs cannot collide even though
#: both are CMACs under a derived key.
#:
#: 0x0000 is excluded because it is the ANT wildcard and a master cannot own it,
#: and a collision is answered by rederiving with a 1-byte suffix 0x00, 0x01,
#: ... - of which a searching keyholder tries COMPAT_LOCATOR_TRIES before
#: falling back to a wildcard search on the private device type.
COMPAT_LOCATOR_LABEL = b"priv"
COMPAT_LOCATOR_WILDCARD = 0x0000
COMPAT_LOCATOR_TRIES = 4
COMPAT_LOCATOR_BITS = 16

#: The bound on the walk: the unsuffixed derivation plus every 1-byte suffix.
#: Reaching it needs 256 consecutive zeros under one key, so it is a bug rather
#: than a collision - and it is a bound rather than a `while True`.
COMPAT_LOCATOR_WALK = 257


def compat_locator_derivation(k_id: bytes, epoch: int, derivation: int) -> int:
    """One candidate: trunc16( CMAC(K_id, "priv" || epoch[4 LE] [|| suffix]) ).

    `derivation` 0 is unsuffixed; 1, 2, 3, ... append the suffix bytes 0x00,
    0x01, 0x02, ... The result is read LITTLE-ENDIAN, which is the same choice
    the beacon's locator field already made: a device number goes on the air low
    byte first, so the two bytes on the air ARE the first two bytes of the tag in
    tag order and a capture can be checked against a CMAC without reversing
    anything.

    This is the raw candidate INCLUDING the excluded wildcard.
    compat_private_locator() is the sequence a node and a searcher both walk.
    """
    if not 0 <= derivation < COMPAT_LOCATOR_WALK:
        raise ValueError("derivation %r is outside the bounded walk"
                         % (derivation,))
    message = COMPAT_LOCATOR_LABEL + (epoch & 0xFFFFFFFF).to_bytes(4, "little")
    if derivation > 0:
        message += bytes([(derivation - 1) & 0xFF])
    return int.from_bytes(cmac(k_id, message)[:COMPAT_LOCATOR_BITS // 8],
                          "little")


def compat_private_locator(k_id: bytes, epoch: int, attempt: int = 0) -> int:
    """Where the node goes when it switches, as a device number.

    Any holder of the root computes this from the epoch it already has, which is
    what makes the SWITCH announcement an OPTIMISATION THAT SAVES A SEARCH
    rather than the only way to find the node: a receiver enrolled after the
    switch finds a node that is already private with nothing to have missed, and
    an observer without the key cannot predict the number at all.

    `attempt` walks the candidates a searcher tries in order. TWO RULES ARE
    FOLDED INTO ONE WALK: the excluded 0x0000 and the collision suffix are two
    different reasons to move to the next derivation, so the sequence is
    "unsuffixed, then suffix 0x00, 0x01, ..., with any derivation that comes out
    0x0000 dropped". Both ends therefore skip the same candidate - a node and a
    searcher that disagreed about whether a zero counts would disagree about
    every candidate after it.
    """
    found = 0
    for derivation in range(COMPAT_LOCATOR_WALK):
        devnum = compat_locator_derivation(k_id, epoch, derivation)
        if devnum == COMPAT_LOCATOR_WILDCARD:
            continue
        if found == attempt:
            return devnum
        found += 1
    raise ValueError("256 consecutive wildcard derivations under one key; "
                     "that is a bug in the key, not a collision")


def tag_byte_index(counter: int, window: int) -> int:
    """Which byte of the window's tag rides in this packet's byte [7].

    Derived from the counter, never from arrival order: by arrival order one
    lost packet desynchronises every window afterwards, permanently, with no
    resync procedure anywhere in the design.
    """
    if window not in LEGAL_W:
        raise ValueError("W must be one of %s, got %d" % (LEGAL_W, window))
    return counter % window


def window_start(counter: int, window: int) -> int:
    """The first counter of the window this packet belongs to."""
    if window not in LEGAL_W:
        raise ValueError("W must be one of %s, got %d" % (LEGAL_W, window))
    return counter - (counter % window)


def expected_index(epoch_us: int, period_counts: int) -> int:
    """The packet index a receiver expects `epoch_us` into the epoch.

    `period_counts` is the ANT channel period in counts of 1/32768 s. The
    receiver derives the counter from TIME rather than from arrival history,
    which is what makes a mid-epoch join, a gap longer than 255 packets and
    sparse mode all work at all.
    """
    if period_counts <= 0:
        raise ValueError("the channel period is 1..65535 counts of 1/32768 s")
    period_us = period_counts * 1000000 // 32768
    return epoch_us // period_us


def resolve_counter(expected: int, counter_low: int) -> tuple:
    """(counter, epoch_delta) from the byte on the air and the expected index.

    Picks the 16-bit rollover nearest the expected index, then reports how many
    counter wraps that implies - because a wrap advances the epoch by 1 on both
    sides, and the receiver's time-derived index carries the high bits for
    free.
    """
    base = expected - (expected & 0xFF)
    best = None
    for candidate in (base + counter_low - 256, base + counter_low,
                      base + counter_low + 256):
        if candidate < 0:
            continue
        if best is None or abs(candidate - expected) < abs(best - expected):
            best = candidate
    if best is None:
        best = counter_low
    return best & 0xFFFF, best >> 16


# ── The hostless node's own derivations ────────────────────────────────────
#
# docs/decisions/0009-hostless-node-identity.md. K_dev is a per-device secret
# provisioned at manufacture; it is the Tier 0 device-number source and the
# pairing-scalar root, and it never goes on the air. These two functions are the
# mirror of src/node/node_ident.c, and they exist so that the C side asserts
# vectors an independent implementation produced rather than asserting that it
# agrees with itself.

#: The pairing label. Four characters, which is exactly what kdf_block() allows,
#: so the ADR's `KDF(K_dev, "pair" || pair_counter)` is this project's existing
#: SP 800-108 block with the counter carried in the epoch field - a fifth label
#: rather than a fifth construction.
LABEL_PAIR = "pair"

#: An X25519 scalar is 32 bytes and the PRF emits 16, so the pairing derivation
#: is the only two-iteration KDF in the project. K_dev is 128 bits, so the
#: scalar carries 128 bits of entropy however many bytes it occupies; that is a
#: property of the root and not of this expansion, and it is stated rather than
#: implied because a 32-byte output invites the assumption of 256-bit strength.
NODE_PAIR_SCALAR_BYTES = 32


def node_pair_block(i: int, pair_counter: int, base_devnum: int) -> bytes:
    """i || "pair" || 0x00 || pair_counter[4 LE] || base_devnum[2 LE] || 0x0100.

    kdf_block()'s layout with two fields changed, and both changes are the
    reason it is a separate function rather than a call to it: the SP 800-108
    counter runs 1..2 instead of being pinned at 1, and [L]_2 is 256 rather than
    128. Deriving 32 bytes by calling a 128-bit KDF twice with the same block
    would return the same 16 bytes twice, so the counter has to reach the PRF.
    """
    if i not in (1, 2):
        raise ValueError("the SP 800-108 counter runs 1..2 for a 256-bit output")
    return (
        bytes([i])
        + LABEL_PAIR.encode("ascii")
        + b"\x00"
        + (pair_counter & 0xFFFFFFFF).to_bytes(4, "little")
        + (base_devnum & 0xFFFF).to_bytes(2, "little")
        + b"\x01\x00"
    )


def node_pair_scalar(k_dev: bytes, pair_counter: int,
                     base_devnum: int = 0) -> bytes:
    """The deterministic X25519 private scalar for one pairing window.

    Unclamped: RFC 7748 clamping happens inside the X25519 primitive, on a copy,
    so clamping here would only mean two implementations of one rule.
    """
    return b"".join(
        cmac(k_dev, node_pair_block(i, pair_counter, base_devnum))
        for i in (1, 2)
    )


def node_tier0_devnum(k_dev: bytes) -> int:
    """The identity Tier 0 device number, 1..65535.

    Tier 0 is the fixed-number tier, and "fixed" here means derived rather than
    stored: a node that can recompute its device number from K_dev cannot lose
    it, and a factory reset that re-rolls K_dev re-rolls the number with it,
    which is the property ADR 0006 wants and a stored number does not have.

    0 is the ANT wildcard, so the range is 1..65535 - the same range
    tools/ant_identity.py's random_device_number() draws from, reached by
    reduction instead of by rejection because a node deriving this at boot
    cannot afford an unbounded retry.
    """
    block = kdf(k_dev, LABEL_ID, 0, 0)
    return (int.from_bytes(block[:2], "little") % 65535) + 1
