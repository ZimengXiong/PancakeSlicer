package resolve

import (
	"embed"
	"encoding/json"
	"math"
	"strconv"
	"strings"
	"sync"
)

//go:embed flushdata/*.txt
var flushData embed.FS

const (
	minFlushVolumeFromSupport = 700
	flushVolumeToSupport      = 230
	MaxFlushVolume = 20000
)

type rgb [3]float64

func f32(x float64) float64 { return float64(float32(x)) }

func parseColour(hex string) (rgb, float64) {
	h := strings.TrimPrefix(strings.TrimSpace(hex), "#")
	if (len(h) != 6 && len(h) != 8) || strings.Trim(strings.ToLower(h), "0123456789abcdef") != "" {
		return rgb{}, 255
	}
	n, _ := strconv.ParseUint(h[:6], 16, 32)
	alpha := 255.
	if len(h) == 8 {
		a, _ := strconv.ParseUint(h[6:], 16, 8)
		alpha = float64(a)
	}
	return rgb{float64(n >> 16 & 255), float64(n >> 8 & 255), float64(n & 255)}, alpha
}

func rgbToLab(c rgb) [3]float64 {
	gamma := func(x float64) float64 {
		if x > 0.04045 {
			return math.Pow((x+0.055)/1.055, 2.4)
		}
		return x / 12.92
	}
	r, g, b := gamma(c[0]/255)*100, gamma(c[1]/255)*100, gamma(c[2]/255)*100
	x := 0.412453*r + 0.35758*g + 0.180423*b
	y := 0.212671*r + 0.71516*g + 0.072169*b
	z := 0.019334*r + 0.119193*g + 0.950227*b
	threshold := f32(0.008856)
	ff := func(t float64) float64 {
		if t > threshold {
			return math.Pow(t, 1./3)
		}
		return 7.787*t + 0.137931
	}
	xn, yn, zn := ff(x/95.0489), ff(y/100), ff(z/108.884)
	return [3]float64{116*yn - 16, 500 * (xn - yn), 200 * (yn - zn)}
}

func deltaE00(c1, c2 rgb) float64 {
	lab1, lab2 := rgbToLab(c1), rgbToLab(c2)
	l1, a1, b1, l2, a2, b2 := lab1[0], lab1[1], lab1[2], lab2[0], lab2[1], lab2[2]
	rad := func(d float64) float64 { return d * math.Pi / 180 }
	p25 := math.Pow(25, 7)
	C1, C2 := math.Hypot(a1, b1), math.Hypot(a2, b2)
	cm7 := math.Pow((C1+C2)/2, 7)
	G := 0.5 * (1 - math.Sqrt(cm7/(cm7+p25)))
	pa1, pa2 := (1+G)*a1, (1+G)*a2
	pc1, pc2 := math.Hypot(pa1, b1), math.Hypot(pa2, b2)
	hue := func(a, b float64) float64 {
		if a == 0 && b == 0 {
			return 0
		}
		return math.Mod(math.Atan2(b, a)+2*math.Pi, 2*math.Pi)
	}
	ph1, ph2 := hue(pa1, b1), hue(pa2, b2)
	dL, dC, dH := l2-l1, pc2-pc1, 0.
	if pc1*pc2 != 0 {
		dH = ph2 - ph1
		if dH < -math.Pi {
			dH += 2 * math.Pi
		} else if dH > math.Pi {
			dH -= 2 * math.Pi
		}
		dH = 2 * math.Sqrt(pc1*pc2) * math.Sin(dH/2)
	}
	Lm, Cm, Hm := (l1+l2)/2, (pc1+pc2)/2, ph1+ph2
	if pc1*pc2 != 0 {
		if math.Abs(ph1-ph2) <= math.Pi {
			Hm /= 2
		} else if Hm < 2*math.Pi {
			Hm = (Hm + 2*math.Pi) / 2
		} else {
			Hm = (Hm - 2*math.Pi) / 2
		}
	}
	T := 1 - 0.17*math.Cos(Hm-rad(30)) + 0.24*math.Cos(2*Hm) + 0.32*math.Cos(3*Hm+rad(6)) - 0.2*math.Cos(4*Hm-rad(63))
	dTheta := rad(30) * math.Exp(-math.Pow((Hm-rad(275))/rad(25), 2))
	RC := 2 * math.Sqrt(math.Pow(Cm, 7)/(math.Pow(Cm, 7)+p25))
	SL := 1 + 0.015*math.Pow(Lm-50, 2)/math.Sqrt(20+math.Pow(Lm-50, 2))
	SC, SH := 1+0.045*Cm, 1+0.015*Cm*T
	RT := -math.Sin(2*dTheta) * RC
	return f32(math.Sqrt(math.Pow(dL/SL, 2) + math.Pow(dC/SC, 2) + math.Pow(dH/SH, 2) + RT*(dC/SC)*(dH/SH)))
}

type flushPredictor struct {
	colours []rgb
	table   map[string]float64
	valid   bool
}

func newFlushPredictor(text string) *flushPredictor {
	p := &flushPredictor{table: map[string]float64{}}
	lines := strings.Split(strings.ReplaceAll(text, "\r\n", "\n"), "\n")
	hex := func(s string) bool { return len(s) == 7 && s[0] == '#' && strings.Trim(strings.ToLower(s[1:]), "0123456789abcdef") == "" }
	if len(lines) < 2 {
		return p
	}
	for _, s := range strings.Fields(lines[1]) {
		if !hex(s) {
			return p
		}
		c, _ := parseColour(s)
		p.colours = append(p.colours, c)
	}
	if len(p.colours) == 0 {
		return p
	}
	for _, line := range lines[min(3, len(lines)):] {
		fields := strings.Fields(line)
		if len(fields) == 0 {
			continue
		}
		if len(fields) < 3 || !hex(fields[0]) || !hex(fields[1]) {
			return p
		}
		v, err := strconv.ParseFloat(fields[2], 64)
		if err != nil {
			return p
		}
		p.table[strings.ToUpper(fields[0]+fields[1])] = f32(v)
	}
	p.valid = true
	return p
}

func (p *flushPredictor) predict(from, to rgb) (float64, bool) {
	if !p.valid {
		return 0, false
	}
	similar := func(c rgb) *rgb {
		for i := range p.colours {
			if deltaE00(p.colours[i], c) <= 5 {
				return &p.colours[i]
			}
		}
		return nil
	}
	a, b := similar(from), similar(to)
	if a == nil || b == nil {
		return 0, false
	}
	key := func(c rgb) string { return strings.ToUpper("#" + hex2(c[0]) + hex2(c[1]) + hex2(c[2])) }
	v, ok := p.table[key(*a)+key(*b)]
	return v, ok
}

func hex2(v float64) string { s := strconv.FormatInt(int64(v), 16); return strings.Repeat("0", 2-len(s)) + s }

var predictors sync.Map

func predictorOf(dataset int) *flushPredictor {
	if p, ok := predictors.Load(dataset); ok {
		return p.(*flushPredictor)
	}
	file := map[int]string{0: "flush_data_standard.txt", 1: "flush_data_dual_standard.txt", 2: "flush_data_dual_highflow.txt"}[dataset]
	text := ""
	if file != "" {
		b, _ := flushData.ReadFile("flushdata/" + file)
		text = string(b)
	}
	p, _ := predictors.LoadOrStore(dataset, newFlushPredictor(text))
	return p.(*flushPredictor)
}

func rgbToHsv(r, g, b float64) (float64, float64, float64) {
	cmax, cmin := math.Max(r, math.Max(g, b)), math.Min(r, math.Min(g, b))
	delta := f32(cmax - cmin)
	var h float64
	switch {
	case math.Abs(delta) < 0.001:
		h = 0
	case cmax == r:
		h = f32(60 * f32(math.Mod(f32((g-b)/delta), 6)))
	case cmax == g:
		h = f32(60 * f32((b-r)/delta+2))
	default:
		h = f32(60 * f32((r-g)/delta+4))
	}
	s := 0.
	if math.Abs(cmax) >= 0.001 {
		s = f32(delta / cmax)
	}
	return h, s, cmax
}

func luminance(r, g, b float64) float64 { return f32(r*0.3 + g*0.59 + b*0.11) }
func toRad(deg float64) float64        { return f32(deg / 180 * f32(math.Pi)) }

func deltaHS(h1, s1, v1, h2, s2, v2 float64) float64 {
	a, b := toRad(h1), toRad(h2)
	dx := f32(f32(math.Cos(a))*s1*v1 - math.Cos(b)*s2*v2)
	dy := f32(f32(math.Sin(a))*s1*v1 - math.Sin(b)*s2*v2)
	return math.Min(1.2, f32(math.Sqrt(dx*dx+dy*dy)))
}

func flushRGB(src, dst rgb, dataset int) float64 {
	if dataset == 0 {
		if p, ok := predictorOf(0).predict(src, dst); ok {
			return math.Trunc(p)
		}
	}
	sr, sg, sb := f32(src[0]/255), f32(src[1]/255), f32(src[2]/255)
	dr, dg, db := f32(dst[0]/255), f32(dst[1]/255), f32(dst[2]/255)
	fh, fs, fv := rgbToHsv(sr, sg, sb)
	th, ts, tv := rgbToHsv(dr, dg, db)
	hsDist := deltaHS(fh, fs, fv, th, ts, tv)
	fromLumi, toLumi := luminance(sr, sg, sb), luminance(dr, dg, db)
	var lumiFlush float64
	if toLumi >= fromLumi {
		lumiFlush = f32(f32(math.Pow(f32(toLumi-fromLumi), 0.7)) * 560)
	} else {
		lumiFlush = f32((fromLumi - toLumi) * 80)
		hsDist = math.Min(f32(0.67*tv+0.33*fv), hsDist)
	}
	hsFlush := f32(230 * hsDist)
	third := f32(math.Sqrt(hsFlush*hsFlush + lumiFlush*lumiFlush - 2*hsFlush*lumiFlush*f32(math.Cos(toRad(120)))))
	return math.Trunc(math.Max(third, 60))
}

func calcFlushVol(from, to string, minFlush float64, dataset int) float64 {
	src, aa := parseColour(from)
	dst, ba := parseColour(to)
	if aa == 0 {
		src = rgb{255, 255, 255}
	}
	if ba == 0 {
		dst = rgb{255, 255, 255}
	}
	if dataset != 0 {
		if p, ok := predictorOf(dataset).predict(src, dst); ok {
			return math.Min(math.Trunc(p), MaxFlushVolume)
		}
	}
	vol := flushRGB(src, dst, dataset)
	fromDark := luminance(src[0], src[1], src[2]) > 180./255
	toLight := luminance(dst[0], dst[1], dst[2]) < 75./255
	if dataset != 0 && fromDark && toLight {
		vol = f32(vol * 1.3)
	}
	vol = f32(vol + minFlush)
	return math.Min(math.Trunc(vol), MaxFlushVolume)
}

func configAt(config map[string]json.RawMessage, key string, i int) string {
	list := ConfigList(config[key])
	if len(list) == 0 {
		return ""
	}
	return strings.TrimSpace(list[min(i, len(list)-1)])
}

func configNumber(config map[string]json.RawMessage, key string, i int, otherwise float64) float64 {
	v, err := strconv.ParseFloat(configAt(config, key, i), 64)
	if err != nil || math.IsNaN(v) || math.IsInf(v, 0) {
		return otherwise
	}
	return v
}

func minFlushVolumes(printer map[string]json.RawMessage, filaments []map[string]json.RawMessage, nozzle int) []float64 {
	nozzleVolume := math.Trunc(configNumber(printer, "nozzle_volume", nozzle, 0))
	level := configNumber(printer, "enable_long_retraction_when_cut", 0, 0)
	activated := configNumber(printer, "long_retractions_when_cut", nozzle, 0) == 1
	printerCut := configNumber(printer, "retraction_distances_when_cut", nozzle, 18)
	out := make([]float64, len(filaments))
	for i, fc := range filaments {
		retract := 0.
		if level != 0 && activated {
			retract = printerCut
		}
		filActivated := configNumber(fc, "filament_long_retractions_when_cut", 0, 0)
		filCut := configAt(fc, "filament_retraction_distances_when_cut", 0)
		if filActivated == 0 {
			retract = 0
		} else if filActivated == 1 && level == 2 {
			retract = printerCut
			if filCut != "" && filCut != "nil" {
				retract = math.Trunc(configNumber(fc, "filament_retraction_distances_when_cut", 0, 18))
			}
		}
		out[i] = math.Trunc(nozzleVolume - math.Pi*1.75*1.75/4*retract)
	}
	return out
}

func isSupportFilament(filaments []map[string]json.RawMessage, i int) bool {
	typeOf := func(c map[string]json.RawMessage) string { return configAt(c, "filament_type", 0) }
	has := func(types ...string) bool {
		for _, c := range filaments {
			for _, t := range types {
				if typeOf(c) == t {
					return true
				}
			}
		}
		return false
	}
	switch t := typeOf(filaments[i]); {
	case t == "PETG" && has("PLA"):
		return true
	case t == "PLA" && has("PETG", "TPU", "TPU-AMS"):
		return true
	}
	v := configAt(filaments[i], "filament_is_support", 0)
	return v == "1" || v == "true"
}

func FlushTable(colours []string, printer map[string]json.RawMessage, filaments []map[string]json.RawMessage, nozzle int) []float64 {
	n := len(colours)
	dataset := int(configNumber(printer, "nozzle_flush_dataset", nozzle, 0))
	mins := minFlushVolumes(printer, filaments, nozzle)
	out := make([]float64, 0, n*n)
	for i := 0; i < n; i++ {
		for j := 0; j < n; j++ {
			switch {
			case i == j:
				out = append(out, 0)
			case isSupportFilament(filaments, j):
				out = append(out, flushVolumeToSupport)
			default:
				v := calcFlushVol(colours[i], colours[j], mins[i], dataset)
				if isSupportFilament(filaments, i) {
					v = math.Max(minFlushVolumeFromSupport, v)
				}
				out = append(out, v)
			}
		}
	}
	return out
}

func MinFlushVolume(printer map[string]json.RawMessage, filaments []map[string]json.RawMessage, nozzle int) float64 {
	low := math.Inf(1)
	if p := predictorOf(int(configNumber(printer, "nozzle_flush_dataset", nozzle, 0))); p.valid {
		for _, v := range p.table {
			low = math.Min(low, math.Trunc(v))
		}
	}
	for _, v := range minFlushVolumes(printer, filaments, nozzle) {
		low = math.Min(low, v)
	}
	return low
}
