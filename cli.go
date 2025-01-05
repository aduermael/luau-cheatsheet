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
    // Create luau directory if it doesn't exist
    if err := os.MkdirAll("luau", 0755); err != nil {
        return err
    }

    // Download latest release
    resp, err := http.Get("https://github.com/luau-lang/luau/archive/refs/heads/master.zip")
    if err != nil {
        return err
    }
    defer resp.Body.Close()

    // Create temporary zip file
    tmpZip := filepath.Join("luau", "master.zip")
    f, err := os.Create(tmpZip)
    if err != nil {
        return err
    }
    defer f.Close()

    if _, err := io.Copy(f, resp.Body); err != nil {
        return err
    }

    // Extract zip file
    r, err := zip.OpenReader(tmpZip)
    if err != nil {
        return err
    }
    defer r.Close()

    for _, f := range r.File {
        rc, err := f.Open()
        if err != nil {
            return err
        }

        path := filepath.Join("luau", strings.TrimPrefix(f.Name, "luau-master/"))
        if f.FileInfo().IsDir() {
            os.MkdirAll(path, 0755)
        } else {
            os.MkdirAll(filepath.Dir(path), 0755)
            outFile, err := os.Create(path)
            if err != nil {
                rc.Close()
                return err
            }
            io.Copy(outFile, rc)
            outFile.Close()
        }
        rc.Close()
    }

    // Cleanup zip file
    os.Remove(tmpZip)

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

    return nil
}

func buildProject() error {
    // Check if main.cpp exists
    if _, err := os.Stat("main.cpp"); os.IsNotExist(err) {
        return fmt.Errorf("main.cpp not found")
    }

    // Build command varies by platform
    var cmd *exec.Cmd
    if runtime.GOOS == "windows" {
        // Build Luau sources
        cmd = exec.Command("cl", "/EHsc",
            "luau/VM/src/*.cpp",
            "luau/Ast/src/*.cpp",
            "luau/Compiler/src/*.cpp",
            "luau/Common/src/*.cpp",
            "main.cpp",
            "/I", "luau/Common/include",
            "/I", "luau/Ast/include",
            "/I", "luau/Compiler/include",
            "/I", "luau/VM/include")
    } else {
    // Check if libluau.a exists
    if _, err := os.Stat("luau/libluau.a"); os.IsNotExist(err) {
        // Build Luau library
        files, err := filepath.Glob("luau/VM/src/*.cpp")
        if err != nil {
            return err
        }
        files2, err := filepath.Glob("luau/Ast/src/*.cpp")
        if err != nil {
            return err
        }
        files3, err := filepath.Glob("luau/Compiler/src/*.cpp")
        if err != nil {
            return err
        }
        files4, err := filepath.Glob("luau/Common/src/*.cpp")
        if err != nil {
            return err
        }

        args := []string{"-c", "-std=c++17"}
        args = append(args, files...)
        args = append(args, files2...)
        args = append(args, files3...)
        args = append(args, files4...)
        args = append(args, "-I", "luau/Common/include")
        args = append(args, "-I", "luau/Ast/include")
        args = append(args, "-I", "luau/Compiler/include")
        args = append(args, "-I", "luau/VM/include")

        cmdLib := exec.Command("g++", args...)
        cmdLib.Stdout = os.Stdout
        cmdLib.Stderr = os.Stderr

        if err := cmdLib.Run(); err != nil {
            return err
        }

        // Create static library
        objFiles, err := filepath.Glob("*.o")
        if err != nil {
            return err
        }

        arCmd := exec.Command("ar", append([]string{"rcs", "luau/libluau.a"}, objFiles...)...)
        arCmd.Stdout = os.Stdout
        arCmd.Stderr = os.Stderr

        if err := arCmd.Run(); err != nil {
            return err
        }

        // Clean up object files
        for _, obj := range objFiles {
            os.Remove(obj)
        }
    }

    // Build main program
    args := []string{"-o", "main", "-std=c++17"}
    args = append(args, "main.cpp")
    args = append(args, "-I", "luau/Common/include")
    args = append(args, "-I", "luau/Ast/include")
    args = append(args, "-I", "luau/Compiler/include")
    args = append(args, "-I", "luau/VM/include")
    args = append(args, "luau/libluau.a")
    cmd = exec.Command("g++", args...)
    }

    cmd.Stdout = os.Stdout
    cmd.Stderr = os.Stderr

    return cmd.Run()
}
