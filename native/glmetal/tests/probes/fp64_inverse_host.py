"""Run emitted scalar integer arithmetic and cofactor bodies as host C++."""
from pathlib import Path
import re, sys
work=Path(sys.argv[1]);source=(work/'inverse3_0_normal.metal').read_text()
scalar=source[source.index('inline bool glm_fp64_arith_nan'):source.index('inline ulong2 glm_fp64_add')]
helpers=''
for n in (3,4):
 start=source.index(f'inline glm_fp64_mat{n}x{n} glm_fp64_inverse')
 end=source.index('); }',start)+4
 helpers+=source[start:end]+'\n'
text='''#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
using ulong=uint64_t;using uint=uint32_t;
#define thread
#define constant
#define device
template<int N>struct V {ulong v[N];template<class... T>V(T... x):v{ulong(x)...}{} ulong& operator[](int i){return v[i];}};
using ulong3=V<3>;using ulong4=V<4>;
template<int N>struct M {V<N> c[N];template<class... T>M(T... x):c{x...}{} V<N>&operator[](int i){return c[i];}};
using glm_fp64_mat3x3=M<3>;using glm_fp64_mat4x4=M<4>;
'''+scalar+helpers+'''
ulong encode(double x){ulong v;memcpy(&v,&x,8);return v;}
template<int N>bool check(){
 for(int mode=0;mode<4;++mode){
  double a[N*N]={},e[N*N]={};
  for(int i=0;i<N;++i){a[i*N+i]=mode==0?ldexp(1,i):1;e[i*N+i]=1/a[i*N+i];}
  if(mode==1){a[0]=a[N+1]=e[0]=e[N+1]=0;a[1]=a[N]=e[1]=e[N]=1;}
  if(mode==2){a[N]=.5;e[N]=-.5;}
  if(mode==3&&N==3){double dense[]={1,.125,.25,.125,65.0/64,5.0/32,.25,5.0/32,69.0/64};double inv[]={4385.0/4096,-49.0/512,-15.0/64,-49.0/512,65.0/64,-.125,-15.0/64,-.125,1};memcpy(a,dense,sizeof dense);memcpy(e,inv,sizeof inv);}
  if(mode==3&&N==4){double dense[]={1,1,1,1,1,-1,1,-1,1,1,-1,-1,1,-1,-1,1};memcpy(a,dense,sizeof dense);for(int c=0;c<N;++c)for(int r=0;r<N;++r)e[c*N+r]=a[r*N+c]/4;}
  M<N> input;for(int c=0;c<N;++c)for(int r=0;r<N;++r)input[c][r]=encode(a[c*N+r]);
  auto output=glm_fp64_inverse(input);
  for(int c=0;c<N;++c)for(int r=0;r<N;++r){double value;auto bits=output[c][r];memcpy(&value,&bits,8);if(value!=e[c*N+r]){printf("Mismatch N=%d mode=%d c=%d r=%d actual=%a expected=%a\\n",N,mode,c,r,value,e[c*N+r]);return false;}}
 }return true;
}
int main(){if(!check<3>()||!check<4>())return 1;puts("Eight generated inverse bodies match exact binary64 references under UBSan");}
'''
(work/'inverse_host.cpp').write_text(text)
