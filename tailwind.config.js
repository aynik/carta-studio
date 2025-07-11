import { createThemes } from 'tw-colors'

/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{vue,js,ts,jsx,tsx}'],
  theme: {
    extend: {},
  },
  plugins: [
    createThemes({
      light: {
        primary: 'rgb(80 80 80)',
        secondary: 'rgb(120 120 120)',
        surface: 'rgb(250 250 250)',
        'surface-alt': 'rgb(240 240 240)',
        'surface-muted': 'rgb(245 245 245)',
        border: 'rgb(220 220 220)',
        'border-muted': 'rgb(235 235 235)',
        text: 'rgb(20 20 20)',
        'text-muted': 'rgb(120 120 120)',
        accent: 'rgb(40 40 40)',
      },
      dark: {
        primary: 'rgb(200 200 200)',
        secondary: 'rgb(160 160 160)',
        surface: 'rgb(20 20 20)',
        'surface-alt': 'rgb(40 40 40)',
        'surface-muted': 'rgb(30 30 30)',
        border: 'rgb(50 50 50)',
        'border-muted': 'rgb(35 35 35)',
        text: 'rgb(240 240 240)',
        'text-muted': 'rgb(160 160 160)',
        accent: 'rgb(255 255 255)',
      },
    }),
  ],
  darkMode: 'class',
}
