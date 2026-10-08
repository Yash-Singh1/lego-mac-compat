"""Exact rational proof for the first CTS inverse_dmat3 argument."""
from fractions import Fraction as F
import json
values = [F(-1023, 2) + F(i, 8) for i in range(9)]
a = values
determinant = a[0]*(a[4]*a[8]-a[7]*a[5])-a[3]*(a[1]*a[8]-a[7]*a[2])+a[6]*(a[1]*a[5]-a[4]*a[2])
assert determinant == 0
print(json.dumps({"case": "KHR-GL41.gpu_shader_fp64.builtin.inverse_dmat3", "column_major_input": [str(v) for v in values], "exact_determinant": str(determinant), "singular": True}))
