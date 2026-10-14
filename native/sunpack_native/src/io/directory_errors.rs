use std::io;
use std::path::Path;

pub(crate) type DirectoryReadError = (String, String, String);
pub(crate) const DIRECTORY_ERROR_REPORT_LIMIT: usize = 100;

/// Diagnostics only: keep successful rows and bound memory on unreadable trees.
pub(crate) struct DirectoryReadErrors {
    scope: String,
    messages: Vec<DirectoryReadError>,
    omitted: usize,
    limit: usize,
    summary_operation: &'static str,
}

impl Default for DirectoryReadErrors {
    fn default() -> Self {
        Self {
            scope: String::new(),
            messages: Vec::new(),
            omitted: 0,
            limit: 20,
            summary_operation: "omitted",
        }
    }
}

impl DirectoryReadErrors {
    pub(crate) fn report() -> Self {
        Self {
            limit: DIRECTORY_ERROR_REPORT_LIMIT,
            summary_operation: "report_omitted",
            ..Self::default()
        }
    }

    pub(crate) fn extend(&mut self, errors: Vec<DirectoryReadError>) {
        for error in errors {
            if self.messages.len() < self.limit && error.0 != "report_omitted" {
                self.messages.push(error);
            } else {
                self.omitted += if matches!(
                    error.0.as_str(),
                    "omitted" | "report_omitted" | "output_scan"
                ) {
                    error.2.parse::<usize>().unwrap_or(1)
                } else {
                    1
                };
            }
        }
    }
    pub(crate) fn new(scope: &Path) -> Self {
        Self {
            scope: scope.to_string_lossy().into_owned(),
            ..Self::default()
        }
    }

    pub(crate) fn push(&mut self, path: &Path, operation: &str, error: io::Error) {
        self.push_detail(path, operation, error.to_string());
    }

    pub(crate) fn push_detail(&mut self, path: &Path, operation: &str, detail: String) {
        if self.messages.len() < self.limit {
            self.messages.push((
                operation.to_string(),
                path.to_string_lossy().into_owned(),
                detail,
            ));
        } else {
            self.omitted += 1;
        }
    }

    pub(crate) fn into_messages(mut self) -> Vec<DirectoryReadError> {
        if self.omitted != 0 {
            self.messages.push((
                self.summary_operation.to_string(),
                self.scope,
                self.omitted.to_string(),
            ));
        }
        self.messages
    }
}
