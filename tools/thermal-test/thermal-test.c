#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define SHENNONG_THERMAL_HOST
#include "../../build/hackios/xnu/files/bsd/dev/shennong_thermal.c"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static void setup(struct th_gov *t){
	memset(t,0,sizeof *t);
	t->cpu_limit=800;t->gpu_limit=800;t->batt_limit=400;t->crit=1050;t->dvfs=true;
	uint32_t tables[4][2]={{364800,2265600},{556800,3148800},{556800,3302400},{556800,3148800}};
	for(int d=0;d<4;d++){struct th_group*g=&t->g[d];g->rows=16;for(int r=0;r<16;r++)g->khz[r]=tables[d][0]+(tables[d][1]-tables[d][0])/15*r;}
	t->gpu_most=12;
	th_init(t);
}
/* plant: per group temp, gpu temp, battery; load given */
static double run(struct th_gov *t,double secs,int load_cpu,int gpu_busy,double *bt,double tc[4],double *tg,int verbose){
	double maxc=0;
	for(long k=0;k<secs*50;k++){
		for(int d=0;d<4;d++)t->g[d].load=load_cpu;
		if(k%5==0){
			for(int d=0;d<4;d++)t->g[d].temp=(int)(tc[d]*10);
			t->gpu_temp=(int)(*tg*10);t->other_temp=(int)((tc[2]+*tg)/2*10);
			int h=t->gpu_temp;for(int d=0;d<4;d++)if(t->g[d].temp>h)h=t->g[d].temp;t->hottest=h;
			th_limits(t);
		}
		if(k%500==0){t->battery=(int)(*bt*10);th_budget(t);}
		th_speed(t);
		uint32_t ga=th_min(t->gpu_cap,t->gpu_ceiling); if(gpu_busy) t->gpu_now=ga; else t->gpu_now=1;
		/* thermal plant: power ~ (f/fmax)^3 */
		double ptot=0;
		for(int d=0;d<4;d++){struct th_group*g=&t->g[d];double f=(double)g->khz[g->row]/g->khz[15];double p=(d==2?6.0:d==0?0.6:3.0)*f*f*f*(load_cpu/100.0)+0.2;
			ptot+=p; double tau=1.5; double target=35+ *bt*0.3 + p*9; tc[d]+= (target-tc[d])*0.02/tau;}
		double fg=gpu_busy? (double)(231+ (903-231)*t->gpu_now/12.0)/903:0.25; double pg=gpu_busy?7.0*fg*fg*fg+0.3:0.3; ptot+=pg;
		*tg += ((35+*bt*0.3+pg*7)-*tg)*0.02/2.0;
		*bt += ((28+ptot*1.6)-*bt)*0.02/180.0;
		if(tc[2]>maxc&&k>50*120)maxc=tc[2];
		t->level=th_level(t);
		if(verbose&&k%(50*60)==0)printf("t=%3lds X4 %.1fC row %u(cap %u ceil %u) A720 %.1f gpu %.1fC idx %u batt %.1f budget %u lvl %d\n",k/50,tc[2],t->g[2].row,t->g[2].cap,t->g[2].ceiling,tc[3],*tg,t->gpu_now,*bt,t->budget,t->level);
	}
	return maxc;
}
int main(){
	struct th_gov t;double tc[4],tg,bt;
	/* 1: idle: rows go to 0 */
	setup(&t); for(int d=0;d<4;d++)t.g[d].load=3; for(int i=0;i<10;i++)th_speed(&t);
	CHECK(t.g[2].row==0,"idle X4 row %u",t.g[2].row);
	/* 2: a touch: 90%% load -> top at once */
	for(int d=0;d<4;d++)t.g[d].load=90; th_speed(&t); CHECK(t.g[2].row==15,"burst row %u",t.g[2].row);
	/* 3: one tick of low load does not drop */
	for(int d=0;d<4;d++)t.g[d].load=5; th_speed(&t); CHECK(t.g[2].row==15,"one low tick dropped to %u",t.g[2].row);
	th_speed(&t);th_speed(&t); CHECK(t.g[2].row<15,"three low ticks stayed %u",t.g[2].row);
	/* 4: 50%% at top -> ~62%% of top */
	setup(&t); t.g[2].load=50; th_speed(&t);th_speed(&t);th_speed(&t); CHECK(t.g[2].khz[t.g[2].row]>=3302400*0.6&&t.g[2].row<15,"50%% -> row %u %u",t.g[2].row,t.g[2].khz[t.g[2].row]);
	/* 5: emergency */
	setup(&t); t.hottest=1060; th_limits(&t); th_speed(&t); CHECK(t.g[2].row==0&&t.gpu_cap==1,"emergency %u %u",t.g[2].row,t.gpu_cap); CHECK(th_level(&t)==4,"lvl");
	/* 6: budget */
	setup(&t); for(int i=0;i<3;i++){t.battery=430+i;th_budget(&t);} CHECK(t.budget==6,"budget %u",t.budget);
	CHECK(t.g[2].khz[t.g[2].ceiling]<=3302400*0.58,"ceiling %u",t.g[2].khz[t.g[2].ceiling]); CHECK(t.gpu_ceiling==6,"gpu ceil %u",t.gpu_ceiling);
	t.battery=370; th_budget(&t); CHECK(t.budget==5,"budget down %u",t.budget);
	t.battery=395; th_budget(&t); CHECK(t.budget==5,"budget hold %u",t.budget);
	/* 7: cooling recovery: no see-saw: one tick under limit-5 doesn't raise */
	setup(&t); t.g[2].temp=860; t.g[2].row=15; th_limits(&t); CHECK(t.g[2].cap==13,"hot cap %u",t.g[2].cap);
	t.g[2].temp=700; for(int i=0;i<9;i++)th_limits(&t); CHECK(t.g[2].cap==13,"early up %u",t.g[2].cap); th_limits(&t); CHECK(t.g[2].cap==14,"up %u",t.g[2].cap);
	/* 8: hot while idle: cap pulled to row+1 only */
	setup(&t); t.g[3].row=2; t.g[3].temp=820; th_limits(&t); CHECK(t.g[3].cap==3,"idle-hot cap %u",t.g[3].cap);
	/* 9: sustained full CPU + GPU load 10 minutes */
	setup(&t); for(int d=0;d<4;d++)tc[d]=60; tg=60; bt=33;
	double m=run(&t,600,100,1,&bt,tc,&tg,1);
	printf("after 10 min: X4 %.1f max %.1f, gpu %.1f, batt %.1f, budget %u\n",tc[2],m,tg,bt,t.budget);
	CHECK(m<88,"X4 peaked %.1f",m); CHECK(tg<86,"gpu %.1f",tg);
	/* 10: 30 minutes */
	m=run(&t,1800,100,1,&bt,tc,&tg,0);
	printf("after 40 min: X4 %.1f max %.1f, gpu %.1f, batt %.1f, budget %u\n",tc[2],m,tg,bt,t.budget);
	CHECK(bt<42.5,"battery %.1f",bt);
	/* 11: idle afterwards recovers */
	run(&t,600,3,0,&bt,tc,&tg,0);
	printf("after 10 min idle: X4 %.1f, batt %.1f, budget %u, cap %u, level %d\n",tc[2],bt,t.budget,t.g[2].cap,t.level);
	CHECK(t.g[2].cap==15&&t.gpu_cap==12,"not recovered: cap %u gpu %u",t.g[2].cap,t.gpu_cap);
	printf(fails?"%d FAILED\n":"all passed\n",fails);
	return fails!=0;
}
