package main

import "fmt"

//go:noinline
func dot(a, b []float64, n int) float64 {
	s := 0.0
	for i := 0; i < n; i++ {
		s += a[i] * b[i]
	}
	return s
}

func main() {
	n := 4000
	a := make([]float64, n)
	b := make([]float64, n)
	for i := 0; i < n; i++ {
		a[i] = float64(i) * 0.5
		b[i] = float64(i) * 0.25
	}
	total := 0.0
	for r := 0; r < 500; r++ {
		total += dot(a, b, n)
	}
	fmt.Println(total)
}
