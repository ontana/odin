INSERT INTO odin.migration VALUES('authz', '004-merge-account-function.blue.sql');

CREATE FUNCTION odin.merge_account_authz(merge_from TEXT, merge_to TEXT)
RETURNS VOID AS
$body$
BEGIN
END;
$body$
LANGUAGE plpgsql SECURITY DEFINER SET search_path = odin;
