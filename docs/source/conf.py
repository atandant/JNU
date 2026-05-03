# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information

project = 'JNU Kernel'
copyright = '2026, Atandant/JNU Authors'
author = 'Atandant/JNU Authors'
release = '0.0.2.2'

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = []

templates_path = ['_templates']
exclude_patterns = []



# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = 'classic'
html_theme_options = {
    'rightsidebar': 'false',
    'stickysidebar': 'true',
    'bgcolor': '#ffffff',
    'sidebarbgcolor': '#f8f9fa',
    'sidebartextcolor': '#222222',
    'sidebarlinkcolor': '#002bb8',
    'relbarbgcolor': '#eaecf0',
    'relbartextcolor': '#000000',
    'relbarlinkcolor': '#002bb8',
    'headbgcolor': '#eaecf0',
    'headtextcolor': '#000000',
    'headlinkcolor': '#002bb8',
    'linkcolor': '#002bb8',
    'visitedlinkcolor': '#5a3696',
    'codebgcolor': '#f8f9fa',
    'bodyfont': 'sans-serif',
    'headfont': 'sans-serif',
}
html_static_path = ['_static']
