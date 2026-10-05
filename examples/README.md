# Examples

Pages that exercise OpenQBrowser's engine. Each one is a plain HTML file with no
build step: open it with the browser and read the source beside the result.

```bash
# From the repository root, after building:
./build/bin/openqbrowser --screenshot=out.png "file://$PWD/examples/basic-layout.html"
```

| File | What it shows |
| --- | --- |
| `basic-layout.html` | Block stacking, margin collapsing, auto margins, borders and padding |
| `inline-text.html` | Line breaking, font sizes and weights, text alignment, inline-blocks |
| `lists-and-tables.html` | List markers, nested lists, table display types |
| `css-cascade.html` | Specificity, inheritance, `!important`, media queries, presentational attributes |
| `interactive-survey.html` | A realistic page: header, navigation, cards, a form and a footer |

## A note on scripting

OpenQBrowser does not run JavaScript, so a page that builds its content in a
script will render only its static markup. `interactive-survey.html` is written
to be useful without scripting and to show what a page loses when scripts do not
run: the form fields are present and styled, but nothing validates them.
