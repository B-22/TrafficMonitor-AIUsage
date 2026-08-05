package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestUpdateIniReplacesKeysInPlace(t *testing.T) {
	in := "; header comment\r\n" +
		"[AIUsage]\r\n" +
		"ShowAntigravity=1\r\n" +
		"\r\n" +
		"[Antigravity]\r\n" +
		"ClientId=old-id\r\n" +
		"ClientSecret=old-secret\r\n" +
		"AccessToken=\r\n" +
		"PrimaryModel=\r\n" +
		"\r\n" +
		"[Kiro]\r\n" +
		"TokenPath=\r\n"
	out := string(updateIniSection([]byte(in), "Antigravity", map[string]string{
		"ClientId":     "new-id",
		"AccessToken":  "ya29.token",
		"RefreshToken": "1//0.refresh",
	}))
	want := "; header comment\r\n" +
		"[AIUsage]\r\n" +
		"ShowAntigravity=1\r\n" +
		"\r\n" +
		"[Antigravity]\r\n" +
		"ClientId=new-id\r\n" +
		"ClientSecret=old-secret\r\n" +
		"AccessToken=ya29.token\r\n" +
		"PrimaryModel=\r\n" +
		"RefreshToken=1//0.refresh\r\n" +
		"\r\n" +
		"[Kiro]\r\n" +
		"TokenPath=\r\n"
	if out != want {
		t.Fatalf("mismatch\n got: %q\nwant: %q", out, want)
	}
}

func TestUpdateIniCreatesSectionWhenMissing(t *testing.T) {
	in := "[AIUsage]\nShowAntigravity=1\n"
	out := string(updateIniSection([]byte(in), "Antigravity", map[string]string{
		"ClientId": "id",
		"AccessToken": "tok",
	}))
	if !strings.Contains(out, "[Antigravity]") {
		t.Fatalf("section not created: %q", out)
	}
	if !strings.Contains(out, "ClientId=id") || !strings.Contains(out, "AccessToken=tok") {
		t.Fatalf("keys missing: %q", out)
	}
	if !strings.Contains(out, "[AIUsage]") || !strings.Contains(out, "ShowAntigravity=1") {
		t.Fatalf("existing content lost: %q", out)
	}
}

func TestUpdateIniCaseInsensitiveSection(t *testing.T) {
	in := "[antigravity]\nAccessToken=\n"
	out := string(updateIniSection([]byte(in), "Antigravity", map[string]string{
		"AccessToken": "new",
	}))
	if !strings.Contains(out, "AccessToken=new") {
		t.Fatalf("case-insensitive section match failed: %q", out)
	}
	if strings.Count(out, "[antigravity]") != 1 {
		t.Fatalf("duplicate section created: %q", out)
	}
}

func TestUpdateIniPreservesCRLF(t *testing.T) {
	in := "[Antigravity]\r\nAccessToken=\r\n[Kiro]\r\n"
	out := string(updateIniSection([]byte(in), "Antigravity", map[string]string{
		"RefreshToken": "rt",
	}))
	if strings.Contains(out, "\n\n") {
		t.Fatalf("unexpected blank line: %q", out)
	}
	if !strings.Contains(out, "\r\n") {
		t.Fatalf("CRLF lost: %q", out)
	}
}

func TestUpdateIniIndentedKey(t *testing.T) {
	in := "[Antigravity]\n  AccessToken =\n"
	out := string(updateIniSection([]byte(in), "Antigravity", map[string]string{
		"AccessToken": "tok",
	}))
	want := "[Antigravity]\n  AccessToken=tok\n"
	if out != want {
		t.Fatalf("indented key handling: got %q want %q", out, want)
	}
}

func TestLocateIniFindsExplicitPath(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "AIUsage.ini")
	if err := os.WriteFile(p, []byte("[Antigravity]\n"), 0644); err != nil {
		t.Fatal(err)
	}
	got, err := locateIni(p)
	if err != nil || got != p {
		t.Fatalf("locateIni(explicit) = %q, %v", got, err)
	}
}

func TestLocateIniMissing(t *testing.T) {
	dir := t.TempDir()
	old := os.Getenv("APPDATA")
	defer os.Setenv("APPDATA", old)
	os.Setenv("APPDATA", filepath.Join(dir, "nonexistent"))
	_, err := locateIni(filepath.Join(dir, "no.ini"))
	if err == nil {
		t.Fatal("expected error for missing ini")
	}
	// The message must guide the user to a fix (double-click next to
	// AIUsage.ini or pass -ini), not just state the failure.
	for _, want := range []string{"-ini", "TrafficMonitor.exe", "AIUsage.ini"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("error should mention %q for actionable guidance, got: %s", want, err)
		}
	}
}
