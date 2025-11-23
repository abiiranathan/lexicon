-- Create RUM extension on database initialization
CREATE EXTENSION IF NOT EXISTS rum;

-- Verify installation
DO $$
DECLARE
    rum_version TEXT;
BEGIN
    SELECT extversion INTO rum_version 
    FROM pg_extension 
    WHERE extname = 'rum';
    
    IF rum_version IS NOT NULL THEN
        RAISE NOTICE 'RUM extension version % installed successfully', rum_version;
    ELSE
        RAISE EXCEPTION 'RUM extension installation failed';
    END IF;
END $$;
