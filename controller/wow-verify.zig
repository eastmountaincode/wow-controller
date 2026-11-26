const Ed = @import("std").crypto.sign.Ed25519;
export fn wow_verify(ptr: [*]const u8, size: usize, sig: *const [64]u8) c_int {
    const key = Ed.PublicKey.fromBytes(@embedFile("owner-public.key").*) catch return 0;
    Ed.Signature.fromBytes(sig.*).verify(ptr[0..size], key) catch return 0;
    return 1;
}
