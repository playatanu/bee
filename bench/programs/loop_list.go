package main

import "fmt"

func run(n int) int {
	xs := make([]int, n)
	for i := 0; i < n; i++ {
		xs[i] = i
	}
	s := 0
	for r := 0; r < 3000; r++ {
		for i := 0; i < n; i++ {
			s += xs[i]
		}
	}
	return s
}

func main() { fmt.Println(run(3000)) }
