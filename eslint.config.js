import globals from 'globals'
import pluginJs from '@eslint/js'
import pluginPrettier from 'eslint-plugin-prettier'
import configPrettier from 'eslint-config-prettier'

export default [
  {
    ignores: ['playground'],
  },
  {
    languageOptions: {
      ecmaVersion: 'latest',
      sourceType: 'module',
      globals: {
        ...globals.node,
        ...globals.jest,
        Blob: 'readonly',
        Worker: 'readonly',
        self: 'readonly',
        importScripts: 'readonly',
        Atrac1: 'readonly',
      },
    },
  },
  pluginJs.configs.recommended,
  {
    plugins: {
      prettier: pluginPrettier,
    },
    rules: {
      ...configPrettier.rules,
      'prettier/prettier': 'error',
    },
  },
]
