use std::io;
use std::path::Path;

pub(crate) type DirectoryReadError = (String, String, String);

/// Diagnostics only: keep successful rows and bound memory on unreadable trees.
#[derive(Default)]
pub(crate) struct DirectoryReadErrors {
    scope: String,
    messages: Vec<DirectoryReadError>,
    omitted: usize,
}

impl DirectoryReadErrors {
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
        if self.messages.len() < 20 {
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
            self.messages
                .push(("omitted".to_string(), self.scope, self.omitted.to_string()));
        }
        self.messages
    }
}
