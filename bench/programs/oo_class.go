package main

import "fmt"

type Point struct{ x, y int }

//go:noinline
func (p *Point) add(o *Point) *Point { p.x += o.x; p.y += o.y; return p }

//go:noinline
func (p *Point) norm() int { return p.x*p.x + p.y*p.y }

func run(n int) int {
	a := &Point{0, 0}
	b := &Point{1, 2}
	s := 0
	for i := 0; i < n; i++ {
		a.add(b)
		s += a.norm()
	}
	return s
}

func main() { fmt.Println(run(400000)) }
