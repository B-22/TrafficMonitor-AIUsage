//go:build !windows

package main

// msgBox is a no-op on non-Windows platforms; console output is used instead.
func msgBox(title, text string, flags uintptr) {}
