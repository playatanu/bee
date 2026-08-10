class P:
    def __init__(self): self.x = 0; self.y = 0
def run(n):
    p = P()
    for i in range(n):
        p.x = p.x + 1
        p.y = p.y + p.x
    return p.y
print(run(2000000))
