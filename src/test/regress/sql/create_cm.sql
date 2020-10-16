-- test storages
CREATE TABLE cmstoragetest(st1 TEXT, st2 INT);
ALTER TABLE cmstoragetest ALTER COLUMN st1 SET STORAGE EXTERNAL;
\d+ cmstoragetest
ALTER TABLE cmstoragetest ALTER COLUMN st1 SET STORAGE MAIN;
\d+ cmstoragetest
ALTER TABLE cmstoragetest ALTER COLUMN st1 SET STORAGE PLAIN;
\d+ cmstoragetest
ALTER TABLE cmstoragetest ALTER COLUMN st1 SET STORAGE EXTENDED;
\d+ cmstoragetest
DROP TABLE cmstoragetest;

CREATE TABLE cmdata(f1 text COMPRESSION pglz);
INSERT INTO cmdata VALUES(repeat('1234567890',1000));
INSERT INTO cmdata VALUES(repeat('1234567890',1001));

-- copy with table creation
SELECT * INTO cmmove1 FROM cmdata;

-- we update using datum from different table
CREATE TABLE cmmove2(f1 text COMPRESSION pglz);
INSERT INTO cmmove2 VALUES (repeat('1234567890',1004));
UPDATE cmmove2 SET f1 = cmdata.f1 FROM cmdata;

-- copy to existing table
CREATE TABLE cmmove3(f1 text COMPRESSION pglz);
INSERT INTO cmmove3 SELECT * FROM cmdata;

-- drop original compression information
DROP TABLE cmdata;

-- check data is okdd
SELECT length(f1) FROM cmmove1;
SELECT length(f1) FROM cmmove2;
SELECT length(f1) FROM cmmove3;

-- zlib compression
CREATE TABLE zlibtest(f1 TEXT COMPRESSION zlib);
CREATE TABLE zlibtest(f1 TEXT COMPRESSION zlib);
CREATE TABLE zlibtest(f1 TEXT COMPRESSION zlib);
INSERT INTO zlibtest VALUES(repeat('1234567890',1004));
INSERT INTO zlibtest VALUES(repeat('1234567890 one two three',1004));
SELECT length(f1) FROM zlibtest;

-- alter compression method with rewrite
ALTER TABLE cmmove2 ALTER COLUMN f1 SET COMPRESSION zlib;
\d+ cmmove2
ALTER TABLE cmmove2 ALTER COLUMN f1 SET COMPRESSION pglz;
\d+ cmmove2

DROP TABLE cmmove1, cmmove2, cmmove3, zlibtest;
