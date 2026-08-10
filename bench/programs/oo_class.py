class Point:
    def __init__(self, x, y): self.x = x; self.y = y
    def add(self, o): self.x += o.x; self.y += o.y; return self
    def norm(self): return self.x * self.x + self.y * self.y
def run(n):
    a = Point(0, 0); b = Point(1, 2); s = 0
    for i in range(n):
        a.add(b); s += a.norm()
    return s
print(run(400000))
