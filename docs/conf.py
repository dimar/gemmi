# -*- coding: utf-8 -*-
import os

# -- General configuration ------------------------------------------------

needs_sphinx = '1.4'

extensions = ['sphinx.ext.doctest', 'sphinx.ext.githubpages']

#templates_path = ['_templates']

source_suffix = '.rst'

master_doc = 'index'

project = u'Gemmi'
copyright = u'2017 Global Phasing Ltd'
author = u'Marcin Wojdyr'

with open('../include/gemmi/version.hpp') as _f:
    for _line in _f:
        if _line.startswith('#define GEMMI_VERSION '):
            version = _line.split()[2].strip('"')
release = version

exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store']
pygments_style = 'sphinx'
todo_include_todos = False
highlight_language = 'c++'


# -- Options for HTML output ----------------------------------------------

if not os.environ.get('READTHEDOCS'):
    html_theme = 'sphinx_rtd_theme'
    #import cloud_sptheme as csp
    #html_theme = "cloud"
    #html_theme_path = [csp.get_theme_dir()]
    #html_theme = 'bizstyle'
    # html_theme_options = {}

#html_static_path = ['_static']


# -- Options for LaTeX output ---------------------------------------------

latex_elements = {
    # 'papersize': 'letterpaper',
    # 'pointsize': '10pt',
    # 'preamble': '',
    # 'figure_align': 'htbp',
}

# Grouping the document tree into LaTeX files. List of tuples
# (source start file, target name, title,
#  author, documentclass [howto, manual, or own class]).
latex_documents = [
    (master_doc, 'Gemmi.tex', u'Gemmi Documentation',
     u'Marcin Wojdyr', 'manual'),
]


# monkey-patch doctest to set the language
import sphinx.ext.doctest  # noqa: E402

def DoctestDirective_run(self):
    nodes = sphinx.ext.doctest.TestDirective.run(self)
    nodes[0]['language'] = 'python'
    return nodes
sphinx.ext.doctest.DoctestDirective.run = DoctestDirective_run
sphinx.ext.doctest.TestcodeDirective.run = DoctestDirective_run
