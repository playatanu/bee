package main

import "fmt"

func run(n, iters int) int {
	m := make([][]int, n)
	for i := 0; i < n; i++ {
		m[i] = make([]int, n)
		for j := 0; j < n; j++ {
			m[i][j] = i * j
		}
	}
	s := 0
	for r := 0; r < iters; r++ {
		for i := 0; i < n; i++ {
			for j := 0; j < n; j++ {
				s += m[i][j]
			}
		}
	}
	return s
}

func main() { fmt.Println(run(200, 80)) }
