/* final7 - card / type pictures (see art.c). */
#ifndef ART_H
#define ART_H

#include <windows.h>
#include <commctrl.h>
#include <stdint.h>

#define ART_ITEMS    725
#define ITEM_DRAGON  723   /* Dragon-type monsters */
#define ITEM_EQUIP   724   /* Equip cards          */
#define ITEM_THUNDER 725   /* Thunder-type monsters */

int        art_init(void);
HBITMAP    art_get(int item);
void       art_draw(HDC dc, int item, int x, int y, int w, int h);
HIMAGELIST art_imagelist(int w, int h);   /* image index = item - 1 */

#endif
