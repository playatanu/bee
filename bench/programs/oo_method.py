class P:
    def __init__(self): self.n = 0
    def one(self): return 1
def run(n):
    p = P(); s = 0
    for i in range(n): s += p.one()
    return s
print(run(3000000))
