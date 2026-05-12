export const THEME_MODES = ["dark", "light"] as const;
export type ThemeMode = (typeof THEME_MODES)[number];

export const THEME_COLORS = [
  "higanRed",
  "coralOrange",
  "sunsetGold",
  "mintGreen",
  "moonlightBlue",
  "twilightPurple",
] as const;
export type ThemeColor = (typeof THEME_COLORS)[number];

export interface AppTheme {
  mode: ThemeMode;
  color: ThemeColor;
}

export const DEFAULT_THEME: AppTheme = {
  mode: "dark",
  color: "higanRed",
};

export function applyTheme(theme: AppTheme): void {
  if (typeof document === "undefined") {
    return;
  }

  document.body.dataset.themeMode = theme.mode;
  document.body.dataset.themeColor = theme.color;
  document.documentElement.style.colorScheme = theme.mode;
}
