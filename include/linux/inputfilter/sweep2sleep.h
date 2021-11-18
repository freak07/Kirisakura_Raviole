#ifndef __S2S_H__
#define __S2S_H__


// if true, TS driver should report fake x and y coords coming from s2s driver, and also pass real x and y to s2s for calc...
extern bool s2s_freeze_coords(int *x, int *y, int r_x, int r_y);


#endif /* __S2S_H__ */
