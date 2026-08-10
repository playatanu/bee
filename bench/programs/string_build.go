package main

import (
	"fmt"
	"strings"
)

func run(n int) (int, int) {
	var b strings.Builder
	for i := 0; i < n; i++ {
		b.WriteString("abcdefghij")
	}
	s := b.String()
	return len(s), len(strings.Split(s, "j"))
}

func main() { a, b := run(300000); fmt.Println(a, b) }
