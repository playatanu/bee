package main

import "fmt"

func run(n int) int {
	xs := make([]int, n)
	for i := 0; i < n; i++ {
		xs[i] = i
	}
	s := 0
	for r := 0; r < 4000; r++ {
		for _, x := range xs {
			s += x
		}
	}
	return s
}

func main() { fmt.Println(run(3000)) }
