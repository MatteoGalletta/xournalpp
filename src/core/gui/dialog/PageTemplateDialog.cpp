#include "PageTemplateDialog.h"

#include <cstdio>    // for sprintf
#include <ctime>     // for localtime, strftime, time
#include <fstream>   // for ofstream, basic_ostream
#include <memory>    // for allocator, unique_ptr
#include <optional>  // for optional
#include <string>    // for string, operator<<

#include <gdk/gdk.h>      // for GdkRGBA
#include <glib-object.h>  // for G_CALLBACK, g_signal_c...

#include "control/pagetype/PageTypeHandler.h"  // for PageTypeInfo, PageType...
#include "control/settings/Settings.h"         // for Settings
#include "control/stockdlg/XojOpenDlg.h"       // for XojOpenDlg
#include "gui/Builder.h"                       // for Builder
#include "gui/PopupWindowWrapper.h"            // for PopupWindowWrapper
#include "gui/menus/popoverMenus/PageTypeSelectionPopoverGridOnly.h"
#include "gui/toolbarMenubar/ToolMenuHandler.h"
#include "model/FormatDefinitions.h"  // for FormatUnits, XOJ_UNITS
#include "model/PageType.h"           // for PageType
#include "util/Color.h"               // for GdkRGBA_to_argb, rgb_t...
#include "util/PathUtil.h"            // for fromGFilename, readString
#include "util/i18n.h"                // for _

#include "FormatDialog.h"  // for FormatDialog
#include "filesystem.h"    // for path

class GladeSearchpath;

constexpr auto UI_FILE = "pageTemplate.glade";
constexpr auto UI_DIALOG_NAME = "templateDialog";

using namespace xoj::popup;

PageTemplateDialog::PageTemplateDialog(GladeSearchpath* gladeSearchPath, Settings* settings, ToolMenuHandler* toolmenu,
                                       PageTypeHandler* types):
        gladeSearchPath(gladeSearchPath), settings(settings), toolMenuHandler(toolmenu), types(types) {
    model.parse(settings->getPageTemplate());

    Builder builder(gladeSearchPath, UI_FILE);
    window.reset(GTK_WINDOW(builder.get(UI_DIALOG_NAME)));

    // Needs to be initialized after this->window
    pageTypeSelectionMenu = std::make_unique<PageTypeSelectionPopoverGridOnly>(types, settings, this);
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(builder.get("btBackgroundDropdown")),
                              pageTypeSelectionMenu->getPopover());

    pageSizeLabel = GTK_LABEL(builder.get("lbPageSize"));
    backgroundTypeLabel = GTK_LABEL(builder.get("lbBackgroundType"));
    backgroundColorChooser = GTK_COLOR_CHOOSER(builder.get("cbBackgroundButton"));
    copyLastPageButton = GTK_TOGGLE_BUTTON(builder.get("cbCopyLastPage"));
    copyLastPageSizeButton = GTK_TOGGLE_BUTTON(builder.get("cbCopyLastPageSize"));


    g_signal_connect_swapped(builder.get("btChangePaperSize"), "clicked",
                             G_CALLBACK(+[](PageTemplateDialog* self) { self->showPageSizeDialog(); }), this);

    g_signal_connect_swapped(builder.get("btLoad"), "clicked",
                             G_CALLBACK(+[](PageTemplateDialog* self) { self->loadFromFile(); }), this);

    g_signal_connect_swapped(builder.get("btSave"), "clicked",
                             G_CALLBACK(+[](PageTemplateDialog* self) { self->saveToFile(); }), this);

    g_signal_connect_swapped(builder.get("btCancel"), "clicked", G_CALLBACK(gtk_window_close), this->getWindow());
    g_signal_connect_swapped(builder.get("btOk"), "clicked", G_CALLBACK(+[](PageTemplateDialog* self) {
                                 self->saveToModel();
                                 self->settings->setPageTemplate(self->model.toString());
                                 self->toolMenuHandler->setDefaultNewPageType(self->model.getPageInsertType());
                                 gtk_window_close(self->getWindow());
                             }),
                             this);

    updateDataFromModel();
}

PageTemplateDialog::~PageTemplateDialog() = default;

void PageTemplateDialog::updateDataFromModel() {
    GdkRGBA color = Util::rgb_to_GdkRGBA(model.getBackgroundColor());
    gtk_color_chooser_set_rgba(backgroundColorChooser, &color);

    updatePageSize();

    pageTypeSelectionMenu->setSelected(model.getBackgroundType());
    changeCurrentPageBackground(types->getInfoOn(model.getBackgroundType()));

    gtk_toggle_button_set_active(copyLastPageButton, model.isCopyLastPageSettings());
    gtk_toggle_button_set_active(copyLastPageSizeButton, model.isCopyLastPageSize());
}

void PageTemplateDialog::changeCurrentPageBackground(const PageTypeInfo* info) {
    model.setBackgroundType(info->page);

    gtk_label_set_text(backgroundTypeLabel, info->name.c_str());
}

void PageTemplateDialog::saveToModel() {
    model.setCopyLastPageSettings(gtk_toggle_button_get_active(copyLastPageButton));
    model.setCopyLastPageSize(gtk_toggle_button_get_active(copyLastPageSizeButton));

    GdkRGBA color;
    gtk_color_chooser_get_rgba(backgroundColorChooser, &color);
    model.setBackgroundColor(Util::GdkRGBA_to_argb(color));
}

void PageTemplateDialog::saveToFile() {
    saveToModel();

    GtkWidget* dialog =
            gtk_file_chooser_dialog_new(_("Save File"), this->getWindow(), GTK_FILE_CHOOSER_ACTION_SAVE, _("_Cancel"),
                                        GTK_RESPONSE_CANCEL, _("_Save"), GTK_RESPONSE_OK, nullptr);

    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), true);

    GtkFileFilter* filterXoj = gtk_file_filter_new();
    gtk_file_filter_set_name(filterXoj, _("Xournal++ template"));
    gtk_file_filter_add_pattern(filterXoj, "*.xopt");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filterXoj);

    if (!settings->getLastSavePath().empty()) {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog),
                                            Util::toGFilename(settings->getLastSavePath()).c_str());
    }

    time_t curtime = time(nullptr);
    char stime[128];
    strftime(stime, sizeof(stime), "%F-Template-%H-%M.xopt", localtime(&curtime));
    std::string saveFilename = stime;

    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), saveFilename.c_str());
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), true);

    gtk_window_set_transient_for(GTK_WINDOW(dialog), this->getWindow());
    if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_OK) {
        gtk_widget_destroy(dialog);
        return;
    }

    auto filepath = Util::fromGFilename(gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)));
    settings->setLastSavePath(filepath.parent_path());
    gtk_widget_destroy(dialog);

    std::ofstream out{filepath};
    out << model.toString();
}

void PageTemplateDialog::loadFromFile() {
    XojOpenDlg dlg(this->getWindow(), this->settings);
    fs::path file = dlg.showOpenTemplateDialog();

    auto contents = Util::readString(file);
    if (!contents.has_value()) {
        return;
    }
    model.parse(*contents);

    updateDataFromModel();
}

void PageTemplateDialog::updatePageSize() {
    const FormatUnits* formatUnit = &XOJ_UNITS[settings->getSizeUnitIndex()];

    char buffer[64];
    sprintf(buffer, "%0.2lf", model.getPageWidth() / formatUnit->scale);
    std::string pageSize = buffer;
    pageSize += formatUnit->name;
    pageSize += " x ";

    sprintf(buffer, "%0.2lf", model.getPageHeight() / formatUnit->scale);
    pageSize += buffer;
    pageSize += formatUnit->name;

    gtk_label_set_text(pageSizeLabel, pageSize.c_str());
}

void PageTemplateDialog::showPageSizeDialog() {
    auto popup = xoj::popup::PopupWindowWrapper<xoj::popup::FormatDialog>(gladeSearchPath, settings,
                                                                          model.getPageWidth(), model.getPageHeight(),
                                                                          [dlg = this](double width, double height) {
                                                                              dlg->model.setPageWidth(width);
                                                                              dlg->model.setPageHeight(height);

                                                                              dlg->updatePageSize();
                                                                          });
    popup.show(this->getWindow());
}

/**
 * The dialog was confirmed / saved
 */
auto PageTemplateDialog::isSaved() const -> bool { return saved; }
