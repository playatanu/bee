def run(n):
    xs = list(range(n)); s = 0
    for r in range(3000):
        for i in range(n):
            if xs[i] % 2 == 0: continue
            s += xs[i]
    return s
print(run(3000))
