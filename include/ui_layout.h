#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

/*
 * ui_layout.h - the phase-15 chrome geometry contract.
 *
 * Pure arithmetic on u32, no includes: the compositor draws the
 * lockscreen numpad and the home launcher grid from these numbers,
 * and the kernel phase-15 selftest (kernel/selftest_ui.c) pushes
 * synthetic touches at the SAME centers to prove unlock -> launcher
 * -> app end to end. Both sides including one header is what keeps
 * the two honest -- do not fork these constants.
 *
 * The canvas is the fixed 800x600 virtio-gpu scanout (drivers/
 * virtio_gpu.c VG_W/VG_H); everything below assumes it.
 *
 * Key/icon grids are row-major. Lock numpad rows: 1..9, then
 * C/0/OK. Launcher rows: Dialer, Messages, Contacts, Clock,
 * Calculator, Settings (see UI_LAUNCH_* in compositor.c).
 */

#define UI_STATUS_H     24u     /* top bar                      */
#define UI_HOME_H       24u     /* bottom home gesture bar      */

/* ---- lockscreen numpad ------------------------------------------------ */

#define UI_KEY_W        96u
#define UI_KEY_H        56u
#define UI_KEY_DX       104u    /* W + 8 gap                    */
#define UI_KEY_DY       64u     /* H + 8 gap                    */
#define UI_NUMPAD_COLS   3u
#define UI_NUMPAD_ROWS   4u
#define UI_NUMPAD_X0    248u    /* (800 - (3*96+2*8)) / 2       */
#define UI_NUMPAD_Y0    200u

#define UI_KEY_CX(col)  (UI_NUMPAD_X0 + (col) * UI_KEY_DX + UI_KEY_W / 2)
#define UI_KEY_CY(row)  (UI_NUMPAD_Y0 + (row) * UI_KEY_DY + UI_KEY_H / 2)

/* ---- home launcher grid ----------------------------------------------- */

#define UI_ICON_W      150u
#define UI_ICON_H       64u
#define UI_ICON_DX     174u    /* W + 24 gap                   */
#define UI_ICON_DY     104u    /* H + 40 gap                   */
#define UI_ICON_COLS    3u
#define UI_ICON_ROWS    2u
#define UI_ICON_X0     151u    /* (800 - (3*150+2*24)) / 2     */
#define UI_ICON_Y0      80u

#define UI_ICON_CX(col) (UI_ICON_X0 + (col) * UI_ICON_DX + UI_ICON_W / 2)
#define UI_ICON_CY(row) (UI_ICON_Y0 + (row) * UI_ICON_DY + UI_ICON_H / 2)

#endif /* UI_LAYOUT_H */
