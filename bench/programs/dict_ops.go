package main

import (
	"fmt"
	"strconv"
)

func run(n int) int {
	d := map[string]int{}
	for i := 0; i < n; i++ {
		d[strconv.Itoa(i%5000)] = i
	}
	s := 0
	for r := 0; r < 4; r++ {
		for i := 0; i < n; i++ {
			s += d[strconv.Itoa(i%5000)]
		}
	}
	return s
}

func main() { fmt.Println(run(200000)) }
