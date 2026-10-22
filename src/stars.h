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
int  stars_autoload(const char *folder);
int  stars_have(void);
int  stars_save(const char *path);
int  stars_load(const char *path);
void stars_draw(HDC dc, int id, int x, int y, int w, int h, COLORREF bg);

#endif
