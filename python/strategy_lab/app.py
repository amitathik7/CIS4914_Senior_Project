"""Strategy Lab. Start with:  streamlit run app.py   (see run_lab.ps1, docs/STRATEGY_LAB.md).

The script only wires modules together. A replay starts in exactly three places, each behind its own explicit button:
`session.launch` (Explore), `compare_session.launch` (Compare) and `validate.run_all` (Validate). Switching views,
stepping, filtering and downloading never start the engine.
"""

import streamlit as st

from strategy_lab_ui import bridge, compare_page, explore, nav, session, theme, validate_page, views
from strategy_lab_ui.errors import LabUiError


def main() -> None:
    st.set_page_config(page_title="Strategy Lab", layout="wide", initial_sidebar_state="expanded")
    st.markdown(theme.CSS, unsafe_allow_html=True)
    theme.warn_if_not_loaded()

    try:
        runner = bridge.find_runner()
        catalog = session.catalog_for(runner)
    except LabUiError as error:
        views.render_setup_error(error)
        return

    view = nav.render()
    if view == nav.COMPARE:
        compare_page.render(runner, catalog)
    elif view == nav.VALIDATE:
        validate_page.render(runner)
    else:
        explore.render(runner, catalog)


main()
