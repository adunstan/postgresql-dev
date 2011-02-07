/* contrib/file_fdw/uninstall_file_fdw.sql */

-- Adjust this setting to control where the objects get created.
SET search_path = public;

-- create wrapper with validator and handler
DROP FOREIGN DATA WRAPPER file_fdw;
DROP FUNCTION file_fdw_handler ();
DROP FUNCTION file_fdw_validator (text[], oid);

