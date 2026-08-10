def dot(a, b, n):
    s = 0.0
    for i in range(n): s += a[i] * b[i]
    return s
def run():
    n = 4000
    a = [i * 0.5 for i in range(n)]
    b = [i * 0.25 for i in range(n)]
    total = 0.0
    for r in range(500): total += dot(a, b, n)
    return total
print(run())
