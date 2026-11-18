package main

import (
	"fmt"

	"github.com/OrcaSlicer/OrcaSlicer/server/resolve"
)

var starterProfiles = []struct {
	id, name, description string
	layers                [2]float64
	values                map[string]string
	heightRule            string
}{
	{"everyday", "Everyday", "Curved tops, balanced speed and strength", [2]float64{0.3, 0.6},
		map[string]string{"nonplanar_mode": "nonplanar_top", "top_shell_layers": "4", "bottom_shell_layers": "3"}, ""},
	{"strong", "Strong", "Four walls, 25 % infill and thicker shells for load-bearing parts", [2]float64{0.4, 0.6},
		map[string]string{"nonplanar_mode": "nonplanar_top", "wall_loops": "4", "sparse_infill_density": "25%", "top_shell_layers": "6", "bottom_shell_layers": "5"}, ""},
	{"fine-detail", "Fine detail", "Thin layers and extra shells for small, detailed parts", [2]float64{0.2, 0.4},
		map[string]string{"nonplanar_mode": "nonplanar_top", "top_shell_layers": "5", "bottom_shell_layers": "4"},
		"fine"},
	{"draft", "Draft", "Flat, thick layers and 10% infill for quick test prints", [2]float64{0.6, 0.7},
		map[string]string{"nonplanar_mode": "off", "top_shell_layers": "3", "bottom_shell_layers": "3", "sparse_infill_density": "10%"},
		"draft"},
}

var starterValues = map[string]string{"wall_loops": "2", "sparse_infill_density": "15%", "sparse_infill_pattern": "grid",
	"top_shell_thickness": "0.8", "bottom_shell_thickness": "0.6",
	"seam_position": "aligned", "support_type": "tree(auto)", "support_style": "default"}

func checkStarters(catalog *resolve.Catalog) error {
	starters := []map[string]any{}
	for i := range starterProfiles {
		starters = append(starters, starterRecord(i))
	}
	if _, err := catalog.ParseLibrary(emptyLibrary("", starters...), nil); err != nil {
		return fmt.Errorf("starter profiles: %w", err)
	}
	return nil
}
