def run(n):
    d = {}
    for i in range(n): d[str(i % 5000)] = i
    s = 0
    for r in range(4):
        for i in range(n): s += d[str(i % 5000)]
    return s
print(run(200000))
