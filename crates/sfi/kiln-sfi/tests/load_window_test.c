/* SPDX-License-Identifier: MIT - Windows parallel-loader ordering model. */
#include "load_window.h"
#include <stdio.h>
typedef struct Model{int lock,observing,scans,loads,bad; } Model;
static uint32_t lock(void* p){((Model*)p)->lock++;return 1;}
static void unlock(void* p){((Model*)p)->lock--;}
static uint32_t observe(void* p){Model* m=p;m->observing=1;return 1;}
static uint32_t scan(void* p){Model* m=p;if(!m->observing||m->lock!=1)m->bad=1;m->scans++;return 1;}
static uint32_t load(void* p){Model* m=p;/* Another thread already owns the Windows load-owner gate and needs loader lock. */if(m->lock){m->bad=1;return 0;}m->loads++;return 1;}
int main(void){Model m={0};SfiLoadWindowOps o={&m,lock,unlock,observe,scan,load,load};uint32_t ok=sfi_run_load_window(&o);if(!ok||m.bad||m.lock||m.loads!=2||m.scans!=3){fprintf(stderr,"FAIL loader-lock inversion: ok%u lock%d loads%d scans%d bad%d\n",ok,m.lock,m.loads,m.scans,m.bad);return 1;}puts("PASS observation precedes scan; loader calls never retain loader lock");return 0;}
