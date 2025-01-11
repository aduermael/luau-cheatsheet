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
	// Create build script
	buildScript := `#!/bin/bash
set -e

# Create build directory
mkdir -p build
cd build

# Run CMake
cmake ..

# Build using all available cores
cmake --build . --parallel $(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)

# Copy executable to parent directory
cp main ..
`

	if err := os.WriteFile("build.sh", []byte(buildScript), 0755); err != nil {
		return fmt.Errorf("failed to write build script: %v", err)
	}

	// Create CMakeLists.txt
	cmakeContents := `cmake_minimum_required(VERSION 3.10)
project(LuauProject)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add Luau subdirectory
add_subdirectory(luau)

# Add executable
add_executable(main main.cpp)

# Link against Luau libraries
target_link_libraries(main PRIVATE 
    Luau.Ast
    Luau.Compiler
    Luau.VM
    Luau.Analysis
    Luau.CodeGen
    Luau.CLI.lib
    isocline
)

# Include directories
target_include_directories(main PRIVATE
    ${CMAKE_SOURCE_DIR}/luau/Common/include
    ${CMAKE_SOURCE_DIR}/luau/Ast/include
    ${CMAKE_SOURCE_DIR}/luau/Compiler/include
    ${CMAKE_SOURCE_DIR}/luau/VM/include
    ${CMAKE_SOURCE_DIR}/luau/Analysis/include
    ${CMAKE_SOURCE_DIR}/luau/CodeGen/include
    ${CMAKE_SOURCE_DIR}/luau/CLI/include
    ${CMAKE_SOURCE_DIR}/luau/CLI/src
    ${CMAKE_SOURCE_DIR}/luau/extern/isocline/include
)
`

	if err := os.WriteFile("CMakeLists.txt", []byte(cmakeContents), 0644); err != nil {
		return fmt.Errorf("failed to write CMakeLists.txt: %v", err)
	}

	// Run the build script
	cmd := exec.Command("/bin/bash", "build.sh")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("build failed: %v", err)
	}

	return nil
}

// Helper function to copy files
func copyFile(src, dst string) error {
	sourceFile, err := os.Open(src)
	if err != nil {
		return err
	}
	defer sourceFile.Close()

	destFile, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer destFile.Close()

	_, err = io.Copy(destFile, sourceFile)
	if err != nil {
		return err
	}

	// Ensure the executable bit is set
	if err := os.Chmod(dst, 0755); err != nil {
		return err
	}

	return nil
}
