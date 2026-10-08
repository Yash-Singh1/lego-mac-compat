#!/usr/bin/env python3
"""Offline formal-epsilon diamond classifier; never chooses a numeric epsilon."""
import argparse
import re
from fractions import Fraction as F

ZERO=(F(0),F(0),F(0));ONE=(F(1),F(0),F(0))
def q(x): return F(int(__import__('math').floor(float(x)*256+.5)),256)
def native(ax,ay,bx,by,cx,cy,xsign):
    ax,ay,bx,by=map(q,(ax,ay,bx,by));cx,cy=F(cx),F(cy)
    # Shift every endpoint by (xsign*epsilon^2, -epsilon).
    starts=[(ax+ay-cx-cy,F(-1),F(xsign)),(ax-ay-cx+cy,F(1),F(xsign))]
    deltas=[bx+by-ax-ay,bx-by-ax+ay];enter,exit=ZERO,ONE
    for s,d in zip(starts,deltas):
        if not d:
            if not (s>(F(-1,2),F(0),F(0)) and s<(F(1,2),F(0),F(0))):return False
            continue
        lo=((F(-1,2)-s[0])/d,-s[1]/d,-s[2]/d)
        hi=((F(1,2)-s[0])/d,-s[1]/d,-s[2]/d)
        if lo>hi:lo,hi=hi,lo
        enter=max(enter,lo);exit=min(exit,hi)
    return enter<exit and exit<ONE and exit>ZERO

def axis_records(path):
    records={}
    for line in open(path):
        m=re.search(r'AxisBoundary width1 axis(\d) reverse(\d) minor([\d.]+) fraction([\d.-]+) short(\d) reconstruct0:(.*)',line)
        if not m:continue
        axis,rev,minor,fraction,short,mask=m.groups();axis,rev,short=map(int,(axis,rev,short));minor=float(minor);fraction=float(fraction)
        low,high=(20+fraction,32+fraction) if not short else ((20.4,20.6) if short==1 else (20.6,20.9))
        a,b=(high,low) if rev else (low,high)
        p=(minor,a,minor,b) if axis else (a,minor,b,minor)
        observed=set()
        for major,values in re.findall(r'(\d+)=([\d,]*)',mask):
            for across in values.strip(',').split(','):
                if across:observed.add((int(across),int(major)) if axis else (int(major),int(across)))
        records[(axis,rev,minor,fraction,short)]=(p,observed,axis)
    return records.values()

def boundary_records(path):
    for line in open(path):
        m=re.search(r'BoundaryNative (diagonal|strip) copy(\d) reverse(\d) axis(\d) shift(-?\d) \((\d+),(\d+)\) covered(\d)',line)
        if not m:continue
        fixture,copy,rev,axis,shift,x,y,actual=m.groups();copy,rev,axis,shift,x,y,actual=map(int,(copy,rev,axis,shift,x,y,actual))
        offset=.5 if copy else -.5
        p=[8+offset,6.4,16+offset,56] if fixture=='diagonal' else [56,12.8+offset,43.2,19.2+offset]
        p[2+axis]+=shift/256
        if rev:p=p[2:]+p[:2]
        yield p,x,y,bool(actual)

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--axis',action='append',default=[]);ap.add_argument('--boundary',action='append',default=[]);args=ap.parse_args()
    for sign in [-1,1]:
        count=bad=0
        for path in args.axis:
            for p,observed,axis in axis_records(path):
                for major in range(18,35):
                    for across in range(2,11):
                        x,y=(across,major) if axis else (major,across)
                        actual=(x,y) in observed;pred=native(*p,F(2*x+1,2),F(2*y+1,2),sign);count+=1
                        if pred!=actual:
                            bad+=1
                            if sign==-1 and bad<=12:print("axis disagreement",p,(x,y),"predicted",pred,"actual",actual)
        for path in args.boundary:
            for p,x,y,actual in boundary_records(path):
                pred=native(*p,F(2*x+1,2),F(2*y+1,2),sign);count+=1
                if pred!=actual:
                    bad+=1
                    if sign==-1 and bad<=12:print("boundary disagreement",p,(x,y),"predicted",pred,"actual",actual)
        print(f'x secondary sign {sign:+d}: {count-bad}/{count} correct, {bad} disagreements')
if __name__=='__main__':main()
