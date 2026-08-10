package main

import "fmt"

// go:noinline keeps this a real call, which is what the benchmark measures --
// otherwise Go inlines it and the comparison is against no call at all.
//
//go:noinline
func pick(xs []int, i int) int {
	a := xs[i]
	return a + 1
}

func run() int {
	xs := make([]int, 1000)
	for i := 0; i < 1000; i++ {
		xs[i] = i
	}
	s := 0
	for r := 0; r < 3000; r++ {
		for i := 0; i < 1000; i++ {
			s += pick(xs, i)
		}
	}
	return s
}

func main() { fmt.Println(run()) }
