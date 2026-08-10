def run(n):
    xs = list(range(n)); s = 0
    for r in range(4000):
        for x in xs: s += x
    return s
print(run(3000))
