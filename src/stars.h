/* final7 - guardian star icons from the game disc (see stars.c). */
#ifndef STARS_H
#define STARS_H

#include <windows.h>
#include <stdint.h>

#define STAR_COUNT 10
/* Game star ids */
enum { STAR_MARS = 1, STAR_JUPITER, STAR_SATURN, STAR_URANUS, STAR_PLUTO, STAR_NEPTUNE,
       STAR_MERCURY, STAR_SUN, STAR_MOON, STAR_VENUS };

int  stars_load_from_disc(const char *path, char *err, int errlen);
int  disc_find_in_folder(const char *folder, char *out, int outlen);   /* a Forbidden Memories image */
int  disc_read_wa(const char *path, uint32_t off, void *buf, uint32_t n); /* bytes of WA_MRG.MRG */
int  stars_autoload(const char *folder);
int  stars_have(void);
int  stars_save(const char *path);
int  stars_load(const char *path);
int  stars_load_mem(const void *data, uint32_t len);   /* the copy built into final7.exe */
void stars_draw(HDC dc, int id, int x, int y, int w, int h, COLORREF bg);

#endif
