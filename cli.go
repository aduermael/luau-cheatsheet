package main

import (
	"archive/zip"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Println("Usage: cli <command>")
		os.Exit(1)
	}

	command := os.Args[1]

	switch command {
	case "deps":
		if err := downloadLuauDeps(); err != nil {
			fmt.Printf("Error downloading dependencies: %v\n", err)
			os.Exit(1)
		}
	case "clean":
		if err := cleanProject(); err != nil {
			fmt.Printf("Error cleaning project: %v\n", err)
			os.Exit(1)
		}
	case "build":
		if err := buildProject(); err != nil {
			fmt.Printf("Error building project: %v\n", err)
			os.Exit(1)
		}
	default:
		fmt.Printf("Unknown command: %s\n", command)
		os.Exit(1)
	}
}

func downloadLuauDeps() error {
	fmt.Println("Downloading Luau dependencies...")

	// Create luau directory if it doesn't exist
	if err := os.MkdirAll("luau", 0755); err != nil {
		return fmt.Errorf("failed to create luau directory: %v", err)
	}

	// Download latest release
	fmt.Println("Downloading master.zip from GitHub...")
	resp, err := http.Get("https://github.com/luau-lang/luau/archive/refs/heads/master.zip")
	if err != nil {
		return fmt.Errorf("failed to download: %v", err)
	}
	defer resp.Body.Close()

	// Create temporary zip file
	tmpZip := filepath.Join("luau", "master.zip")
	f, err := os.Create(tmpZip)
	if err != nil {
		return fmt.Errorf("failed to create temp zip: %v", err)
	}
	defer f.Close()

	if _, err := io.Copy(f, resp.Body); err != nil {
		return fmt.Errorf("failed to write zip: %v", err)
	}

	// Extract zip file
	fmt.Println("Extracting files...")
	r, err := zip.OpenReader(tmpZip)
	if err != nil {
		return fmt.Errorf("failed to open zip: %v", err)
	}
	defer r.Close()

	for _, f := range r.File {
		// Debug print for relevant directories
		if strings.Contains(f.Name, "Config") ||
			strings.Contains(f.Name, "Analysis") ||
			strings.Contains(f.Name, "EqSat") {
			fmt.Printf("Extracting: %s\n", f.Name)
		}

		rc, err := f.Open()
		if err != nil {
			return fmt.Errorf("failed to open file in zip: %v", err)
		}

		path := filepath.Join("luau", strings.TrimPrefix(f.Name, "luau-master/"))
		if f.FileInfo().IsDir() {
			os.MkdirAll(path, 0755)
		} else {
			os.MkdirAll(filepath.Dir(path), 0755)
			outFile, err := os.Create(path)
			if err != nil {
				rc.Close()
				return fmt.Errorf("failed to create output file %s: %v", path, err)
			}
			io.Copy(outFile, rc)
			outFile.Close()
		}
		rc.Close()
	}

	// Cleanup zip file
	os.Remove(tmpZip)

	// Expand our critical files list
	criticalFiles := []string{
		"luau/Config/include/Luau/LinterConfig.h",
		"luau/Analysis/include/Luau/Linter.h",
		"luau/Analysis/include/Luau/Module.h",
		"luau/EqSat/include/Luau/EGraph.h",
		"luau/Analysis/include/Luau/EqSatSimplificationImpl.h",
		"luau/Analysis/include/Luau/BuiltinTypes.h",
		"luau/Analysis/include/Luau/Frontend.h",
		"luau/Analysis/include/Luau/TypeInfer.h",
	}

	fmt.Println("\nVerifying critical files:")
	for _, file := range criticalFiles {
		if _, err := os.Stat(file); os.IsNotExist(err) {
			fmt.Printf("❌ Missing: %s\n", file)
		} else {
			fmt.Printf("✓ Found: %s\n", file)
		}
	}

	// Add a directory listing for Analysis include
	fmt.Println("\nContents of Analysis include directory:")
	files, err := filepath.Glob("luau/Analysis/include/Luau/*")
	if err != nil {
		fmt.Printf("Error listing Analysis directory: %v\n", err)
	} else {
		for _, f := range files {
			fmt.Printf("Found: %s\n", f)
		}
	}

	// Add EqSat directory listing
	fmt.Println("\nContents of EqSat include directory:")
	eqsatFiles, err := filepath.Glob("luau/EqSat/include/Luau/*")
	if err != nil {
		fmt.Printf("Error listing EqSat directory: %v\n", err)
	} else {
		for _, f := range eqsatFiles {
			fmt.Printf("Found: %s\n", f)
		}
	}

	return nil
}

func cleanProject() error {
	// Remove luau directory
	if err := os.RemoveAll("luau"); err != nil {
		return err
	}

	// Remove built binary
	binaryName := "main"
	if runtime.GOOS == "windows" {
		binaryName += ".exe"
	}
	if err := os.Remove(binaryName); err != nil && !os.IsNotExist(err) {
		return err
	}

	// Remove all .o files in project root
	files, err := filepath.Glob("*.o")
	if err != nil {
		return err
	}
	for _, f := range files {
		if err := os.Remove(f); err != nil {
			return err
		}
	}

	return nil
}

func buildProject() error {
	// Run the build script
	cmd := exec.Command("/bin/bash", "build.sh")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("build failed: %v", err)
	}
	return nil
}
