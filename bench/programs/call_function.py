def pick(xs, i):
    a = xs[i]
    return a + 1
def run():
    xs = list(range(1000)); s = 0
    for r in range(3000):
        for i in range(1000): s += pick(xs, i)
    return s
print(run())
