#include "wide_axis_interval.h"
#include <assert.h>
#include <stdio.h>

static void check(double a,double b)
{
    struct glm_probe_axis_interval r;
    if(!glm_probe_wide_axis_interval(a,b,0,64,&r))return;
    a=floor(a*256+.5)/256;b=floor(b*256+.5)/256;
    unsigned expected=0;int first=-1,last=-1;
    for(int pixel=0;pixel<64;++pixel)if(pixel+.5>=fmin(a,b)&&pixel+.5<fmax(a,b)){
        if(first<0)first=pixel;last=pixel;++expected;
    }
    assert(r.count==expected);
    if(!expected)return;
    assert(r.first_pixel==first && r.last_pixel==last);
    assert((a<b)==(r.draw_start<r.draw_end));
    /* For every nondegenerate diamond half-span, a center-start segment
     * exits exactly the desired diamonds. The zero-span boundary convention
     * remains a native-rasterizer invariant for GPU validation. */
    for(int span=1;span<=128;++span){double h=span/256.0;
        for(int pixel=0;pixel<64;++pixel){double exit=pixel+.5+(a<b?h:-h);
            bool covered=a<b?(exit>r.draw_start && exit<r.draw_end):(exit<r.draw_start && exit>r.draw_end);
            assert(covered==(pixel>=first&&pixel<=last));
        }
    }
}
int main(void)
{
    struct glm_probe_axis_interval r;
    const double fractions[]={0,.1,.4,.5,.6,.9,nextafter(.5,0),nextafter(.5,1)};
    unsigned checks=0;
    for(int low=1;low<62;++low)for(int high=low;high<63;++high)
        for(unsigned a=0;a<sizeof fractions/sizeof *fractions;++a)
            for(unsigned b=0;b<sizeof fractions/sizeof *fractions;++b){
                check(low+fractions[a],high+fractions[b]);check(high+fractions[b],low+fractions[a]);checks+=2;
            }
    check(nextafter(20.5,0),nextafter(32.5,0));
    check(nextafter(20.5,64),nextafter(32.5,64));
    check(nextafter(32.5,0),nextafter(20.5,0));
    check(nextafter(32.5,64),nextafter(20.5,64));
    assert(glm_probe_wide_axis_interval(20.4,20.6,0,64,&r)&&r.count==1);
    assert(glm_probe_wide_axis_interval(20.6,20.9,0,64,&r)&&r.count==0);
    assert(glm_probe_wide_axis_interval(20.5,20.5,0,64,&r)&&r.count==0);
    assert(!glm_probe_wide_axis_interval(20.6,.4,0,64,&r)); /* Reverse-safe edge requires fallback. */
    assert(!glm_probe_wide_axis_interval(20.4,63.6,0,64,&r));
    assert(!glm_probe_wide_axis_interval(0,20,0,64,&r));
    assert(!glm_probe_wide_axis_interval(20,64,0,64,&r));
    assert(!glm_probe_wide_axis_interval(INFINITY,20,0,64,&r));
    assert(!glm_probe_wide_axis_interval(20,NAN,0,64,&r));
    assert(!glm_probe_wide_axis_interval(2147483700,2147483650,INT32_MAX-32,64,&r));
    assert(glm_probe_wide_axis_interval(-2147483600.4,-2147483590.6,INT32_MIN,64,&r));
    printf("Axis interval CPU proof passed: %u directed intervals\n",checks);
}
