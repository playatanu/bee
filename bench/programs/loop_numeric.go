package main

import "fmt"

func main() {
	s := 0
	for i := 0; i < 30000; i++ {
		for j := 0; j < 3000; j++ {
			s += j
		}
	}
	fmt.Println(s)
}
