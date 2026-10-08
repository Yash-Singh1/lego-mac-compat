"""Generate exact dense inverse references and norm bounds independently."""
from fractions import Fraction as F
import json

def inverse(a):
    n=len(a)
    augmented=[row+[F(int(r==c)) for c in range(n)] for r,row in enumerate(a)]
    for c in range(n):
        pivot=next(r for r in range(c,n) if augmented[r][c])
        augmented[c],augmented[pivot]=augmented[pivot],augmented[c]
        value=augmented[c][c]
        augmented[c]=[x/value for x in augmented[c]]
        for r in range(n):
            if r!=c:
                value=augmented[r][c]
                augmented[r]=[x-value*y for x,y in zip(augmented[r],augmented[c])]
    return [row[n:] for row in augmented]

lower=[[F(int(r==c)) if r<=c else F(r-c,8) for c in range(3)] for r in range(3)]
dense=[[sum(lower[r][k]*lower[c][k] for k in range(3)) for c in range(3)] for r in range(3)]
hadamard=[[F(x) for x in row] for row in [[1,1,1,1],[1,-1,1,-1],[1,1,-1,-1],[1,-1,-1,1]]]
results=[]
for name,a in [('unit_lower_product3',dense),('hadamard4',hadamard)]:
    inv=inverse(a)
    norm_product=max(sum(abs(x) for x in row) for row in a)*max(sum(abs(x) for x in row) for row in inv)
    assert norm_product<=8
    n=len(a)
    for r in range(n):
        for c in range(n):
            assert sum(a[r][k]*inv[k][c] for k in range(n))==int(r==c)
    results.append(dict(name=name,column_major_input=[str(a[r][c]) for c in range(n) for r in range(n)],column_major_inverse=[str(inv[r][c]) for c in range(n) for r in range(n)],infinity_norm_product=str(norm_product),residual_bound_norm_factor=8))
print(json.dumps(results,indent=2))
