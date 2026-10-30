#pragma once

// The viewport's environment and bloom settings (PreviewLighting's environment fields): read from
// graywolf.json, and the toolbar's controls that change and save them.

struct PreviewLighting;

// The environment, its turn and strength, the backdrop, the bloom: from the settings (preview_environment,
// preview_env_rotation, preview_env_intensity, preview_env_backdrop, preview_env_backdrop_blur,
// preview_bloom, preview_bloom_strength); PreviewLighting's defaults for those not set
void LoadPreviewEnvironmentSettings(PreviewLighting& L);

// The environment's choice and a button whose popup holds its turn, strength, the backdrop and the bloom, on the
// current line (ImGui); saved as they are let go
void DrawPreviewEnvironmentControls(PreviewLighting& L);
