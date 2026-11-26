/* The read-only object layout is shared with the verified playhead observer.
 * Read the active filters, so restarting the engine never leaves a cached
 * guessed state. These fields are set by the native effect handler itself. */
enum { WOW_PANEL_FX_KEY = 512 };
static int performance_keys, pending_effect;

static char current_effect(void) {
    uint32_t deck, tag, reverb, delay;
    unsigned char reverb_on = 0, delay_on = 0;
    if (!performance_keys || !cursor_base ||
        !cursor_field(cursor_base, 0x1a03c, &deck, 4) ||
        !cursor_field(deck, 0, &tag, 4) || tag != cursor_base + 0x19514 ||
        !cursor_field(deck, 0x3d1c, &reverb, 4) ||
        !cursor_field(deck, 0x3d24, &delay, 4) || (!reverb && !delay)) return 'U';
    if (reverb && (!cursor_field(reverb, 0x6c, &reverb_on, 1) || reverb_on > 1)) return 'U';
    if (delay && (!cursor_field(delay, 0x14, &delay_on, 1) || delay_on > 1)) return 'U';
    if (reverb && delay && reverb_on != delay_on) return 'U';
    int mode = (reverb ? 1 : 0) + (delay ? 2 : 0);
    return 'a' + (mode - 1) * 2 + (reverb_on || delay_on);
}
