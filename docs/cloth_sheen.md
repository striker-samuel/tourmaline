# Cloth sheen (The Order: 1886 style)

Cloth is an **opt-in** material effect using the velvet/microfiber specular distribution from *Crafting a Next-Gen Material Pipeline for The Order: 1886* (Neubelt & Pettineo, SIGGRAPH 2013). It replaces the direct-light GGX specular lobe with the modified Ashikhmin–Premože velvet distribution and a smooth visibility approximation, producing a soft grazing-angle fuzz highlight.

## Authoring

- In the editor's Material panel, enable **Cloth sheen** on a lit material.
- Use **Roughness** to set the microfiber lobe width and Base Color to set the fabric's body color. The current control is binary: strength and tint are not separately authored.
- In a native `.material` file, write `clothSheen=1` (default: 0).
- In glTF, add the Tourmaline-specific material extension `"SMVK_materials_clothSheen": {"enabled": true}`. This is **not** an implementation of `KHR_materials_sheen`.

The flag occupies bit 1 of `MaterialValues.emissiveFactor.w`, alongside the sun-contact-shadows bit. It is preserved by edits and material creation without increasing the 128-byte GPU/pak material layout. It does not change alpha modes, texturing, vertex colors, or the shader family.

## Scope / limitations

- The velvet response currently applies to the main sun's **direct specular** lighting. Other GGX-based specular sources (environment probes and GI/reflections) are not velvet-convolved; their contribution remains as before.
- This is a dedicated velvet-style BRDF, **not** a full multilayer woven cloth model, nor a complete reproduction of the proprietary game renderer.
- The effect is suppressed by the far-LOD `litSpecular=0` simplification, as with other specular highlights.
- Recommended visual checks: side-lit velvet sphere, oblique fabric plane, high/low roughness under sun, standard GGX material alongside it, plus editor save/reload and import of tagged/untagged glTF.

Reference: Neubelt and Pettineo, *Crafting a Next-Gen Material Pipeline for The Order: 1886* (SIGGRAPH 2013), velvet distribution/visibility section.
