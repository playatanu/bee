def run(n, iters):
    m = [[i * j for j in range(n)] for i in range(n)]
    s = 0
    for r in range(iters):
        for i in range(n):
            for j in range(n): s += m[i][j]
    return s
print(run(200, 80))
