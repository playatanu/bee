def run(n):
    xs = list(range(n)); s = 0
    for r in range(3000):
        for i in range(n): s += xs[i]
    return s
print(run(3000))
