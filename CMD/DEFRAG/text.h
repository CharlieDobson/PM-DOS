/*
 * TEXT.H - what DEFRAG says.  Every sentence is in TEXT.C, so that the
 * wording is in one place and the screens are about where things go.
 */
#ifndef TEXT_H
#define TEXT_H

extern const char tx_program[];         /* the name in the corner of the screen */
extern const char *const tx_method[3];  /* METHOD_*: what each is called */
extern const char *const tx_method_about[2];
extern const char *const tx_sort[5];    /* SORT_* */
extern const char *const tx_order[2];
extern const char *const tx_fs_error[6];    /* FSE_*, lowest bit first */

/* the menu: an item's label, with '&' before its letter; its key; and
   the line at the bottom of the screen while it is lit */
typedef struct {
    const char *label;          /* NULL: a line across the menu */
    const char *key;
    const char *hint;
    int         go;             /* a GO_* */
} MENUTEXT;
#define MENU_ITEMS      10
extern const MENUTEXT tx_menu[MENU_ITEMS];
extern const char tx_menu_title[];

extern const char tx_hint_menu[];       /* nothing is open */
extern const char tx_hint_button[];     /* a box with buttons is */
extern const char tx_hint_drive[];
extern const char tx_hint_method[];
extern const char tx_hint_sort[];
extern const char tx_hint_legend[];
extern const char tx_hint_help[];
extern const char tx_f1[];              /* the right of the top line */
extern const char tx_esc_stops[];       /* ...while a drive is being optimized */

extern const char tx_status[];          /* the two boxes under the map */
extern const char tx_legend[];
extern const char tx_cluster[];         /* "Cluster %,u" */
extern const char tx_elapsed[];         /* "Elapsed Time: %02u:%02u:%02u" */
extern const char tx_used[], tx_unused[], tx_reading[], tx_writing[], tx_bad[], tx_fixed[];
extern const char tx_scale[];           /* "Drive %c:   1 block = %,u cluster%s" */

extern const char tx_act_read[], tx_act_write[], tx_act_sort[], tx_act_fat[];
extern const char tx_act_done[];

extern const char tx_reading_drive[];   /* the box while a drive is read */
extern const char tx_analyzing[];

extern const char tx_recommend_title[];
extern const char tx_recommend_1[];     /* "%u%% of drive %c: is not fragmented." */
extern const char tx_recommend_2[];
extern const char tx_btn_optimize[], tx_btn_configure[], tx_btn_ok[], tx_btn_cancel[];
extern const char tx_btn_resume[], tx_btn_another[], tx_btn_exit[], tx_btn_scandisk[];
extern const char tx_btn_leave[];

extern const char tx_drive_title[];
extern const char tx_method_title[];
extern const char tx_sort_title[], tx_sort_about[], tx_sort_by[], tx_sort_order[];
extern const char tx_legend_title[];
extern const char *const tx_legend_line[7];
extern const char tx_about_title[];
extern const char *const tx_about_line[3];
extern const char tx_help_title[];
extern const char tx_help[];

extern const char tx_stop_1[], tx_stop_2[];
extern const char tx_finished[];
extern const char tx_done_1[];          /* "Drive %c: has been optimized." */
extern const char tx_done_2[];

extern const char tx_error_title[];
extern const char tx_fs_found[];        /* "DEFRAG found a problem on drive %c:" */
extern const char tx_fs_offer[];
extern const char tx_fs_again[];
extern const char tx_no_scandisk[];
extern const char tx_scandisk_failed[]; /* "%s could not be started (error %u)." */

extern const char tx_bad_drive[];       /* "%c:" in each */
extern const char tx_not_local[];
extern const char tx_exfat[];
extern const char tx_cannot_read[];
extern const char tx_in_use[];
extern const char tx_redirected[];
extern const char tx_test_same_drive[];
extern const char tx_no_free[];
extern const char tx_read_error[];      /* "...cluster %,u..." */
extern const char tx_write_error[];
extern const char tx_system_error[];
extern const char tx_internal[];
extern const char tx_no_memory[];
extern const char tx_no_restart[];

extern const char tx_bad_switch[], tx_too_many[], tx_bad_order[], tx_both_methods[];

#endif
