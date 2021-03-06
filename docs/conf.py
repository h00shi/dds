# -*- coding: utf-8 -*-
# Refer: http://www.sphinx-doc.org/en/master/config

# -- Project information -----------------------------------------------------
project = 'dds'
copyright = '2019, vector-of-bool'
author = 'vector-of-bool'

# The short X.Y version
version = ''
# The full version, including alpha/beta/rc tags
release = '0.1.0'

# -- General configuration ---------------------------------------------------
extensions = []
templates_path = ['_templates']
source_suffix = '.rst'
master_doc = 'index'
language = None
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store']
pygments_style = None

# -- Options for HTML output -------------------------------------------------
html_theme = 'pyramid'
html_theme_options = {}
html_static_path = ['_static']
html_sidebars = {}


def setup(app):
    app.add_stylesheet('tweaks.css')
